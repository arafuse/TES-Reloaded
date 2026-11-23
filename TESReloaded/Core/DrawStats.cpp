#include "Managers.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DrawStats {

	namespace {

		const char* const PassNames[Pass_COUNT] = { "main pass (far pass when the shell is on)", "shell pass", "water reflection pass" };
		const char* const ClassNames[Class_COUNT] = {
			"statics", "distant LOD", "LOD trees", "land", "land LOD", "trees", "grass", "actors", "water", "other"
		};

		/// Order-dependent 64-bit mixer; collisions are negligible at ~1000 keys a frame.
		struct Mix {
			UInt64 H = 0x84222325CBF29CE4ULL;
			void Add(UInt32 V) { H ^= V; H *= 0x9E3779B97F4A7C15ULL; H ^= H >> 29; }
			void Add(const void* P) { Add((UInt32)P); }
		};

		enum Column { Col_Draws, Col_Objects, Col_GeomData, Col_Materials, Col_LightSets, Col_MatLights, Col_Groups, Col_Runs, Col_Lit, Col_COUNT };

		/// Set by NiRenderer_ReflectionPass (0x49BEF0) around the reflection map's scene walk and draws.
		UInt8* const kIsRenderingReflectionPass = (UInt8*)0x00B42CE8;

		/// Main-pass size bins: the draw's world bound diameter on screen, in pixels of screen height,
		/// against the scene camera. Bin i holds [2^(i-1), 2^i) px; bin 0 is under 1 px.
		const int SizeBinCount = 9;
		const char* const SizeBinNames[SizeBinCount] = { "<1", "1-2", "2-4", "4-8", "8-16", "16-32", "32-64", "64-128", ">=128" };
		unsigned int gFrameSizeBins[Class_COUNT][SizeBinCount] = {};
		double       gWindowSizeBins[Class_COUNT][SizeBinCount] = {};

		/// One pass/class this frame: hook calls and the distinct keys they drew with.
		struct FrameSlot {
			unsigned int Draws = 0, Runs = 0, Lit = 0;
			LONGLONG EngineTicks = 0;
			std::unordered_set<const void*> Objects, GeomData;
			std::unordered_set<UInt64> Materials, LightSets, MatLights, Groups;
		};

		/// One pass/class over the report window: per-frame values summed, plus the worst frame's draws.
		struct WindowRow {
			double       Sum[Col_COUNT] = {};
			LONGLONG     EngineTicks = 0;
			unsigned int MaxDraws = 0;
		};

		struct Roots {
			NiNode* Shadow = NULL;
			NiNode* ObjectGrid = NULL;
			NiNode* LandLOD = NULL;
			NiNode* DistantRef = NULL;
			NiNode* LODTrees = NULL;
			NiNode* LODWater = NULL;
		};

		struct PropInfo {
			bool Lighting = false;   // derives from BSShaderLightingProperty, so +0x6C is its light list
			bool SpeedTree = false;
		};

		FrameSlot gFrame[Pass_COUNT][Class_COUNT];
		WindowRow gWindow[Pass_COUNT][Class_COUNT];
		UInt64    gLastGroup[Pass_COUNT] = {};
		Roots     gRoots;
		bool      gRootsValid = false;
		std::unordered_map<const void*, PropInfo> gPropInfo;  // by vtable, so never stale
		std::unordered_map<std::string, unsigned int> gOtherTops;

		bool NameStarts(const char* Name, const char* Prefix) {
			return Name && !strncmp(Name, Prefix, strlen(Prefix));
		}

		NiNode* ChildNamed(NiNode* Parent, const char* Name) {
			if (!Parent) return NULL;
			for (int i = 0; i < Parent->m_children.end; i++) {
				NiAVObject* Child = Parent->m_children.data[i];
				if (Child && Child->m_pcName && !strcmp(Child->m_pcName, Name)) return (NiNode*)Child;
			}
			return NULL;
		}

		/// Scene-graph anchors from memory distant-lod-scene-graph: Tes->LODRoot is the node named
		/// LandLOD, and its parent holds DistantRefLOD and LODWaterRoot.
		void ResolveRoots() {
			gRoots = Roots();
			gRootsValid = true;
			if (!Tes) return;
			gRoots.ObjectGrid = Tes->ObjectLODRoot;
			gRoots.Shadow = gRoots.ObjectGrid ? gRoots.ObjectGrid->m_parent : NULL;
			gRoots.LandLOD = Tes->LODRoot;
			NiNode* LODContainer = gRoots.LandLOD ? gRoots.LandLOD->m_parent : NULL;
			gRoots.DistantRef = ChildNamed(LODContainer, "DistantRefLOD");
			gRoots.LODWater = ChildNamed(LODContainer, "LODWaterRoot");
			gRoots.LODTrees = ChildNamed(gRoots.DistantRef, "LOD Trees");
		}

		const PropInfo& PropertyInfo(NiProperty* Prop) {
			const void* Vtbl = *(const void**)Prop;
			auto It = gPropInfo.find(Vtbl);
			if (It != gPropInfo.end()) return It->second;
			PropInfo Info;
			for (const NiRTTI* Type = Prop->GetType(); Type; Type = Type->parent) {
				if (!Type->name) continue;
				if (!strcmp(Type->name, "BSShaderLightingProperty")) Info.Lighting = true;
				if (NameStarts(Type->name, "SpeedTree")) Info.SpeedTree = true;
			}
			return gPropInfo[Vtbl] = Info;
		}

		/// The draw's light set, independent of list order; 0 when the property carries no light list.
		UInt64 LightSetKey(NiProperty* Prop) {
			BSShaderLightingProperty* Lighting = (BSShaderLightingProperty*)Prop;
			const void* Lights[32];
			int Count = 0;
			for (auto* Entry = Lighting->lights.start; Entry && Count < 32; Entry = Entry->next) Lights[Count++] = Entry->data;
			std::sort(Lights, Lights + Count);
			Mix M;
			M.Add(Lighting->lights.numItems);
			for (int i = 0; i < Count; i++) M.Add(Lights[i]);
			return M.H | 1;
		}

		/// Walks m_parent to the first known anchor; Top receives the shadow scene node's child the
		/// walk ended under when no anchor matched.
		int Classify(NiGeometry* Geometry, NiSkinInstance* SkinInstance, const char* VertexName, const char* PixelName, bool TreeProperty, const char** Top) {
			if (NameStarts(PixelName, "WATER")) return Class_Water;
			if (NameStarts(VertexName, "GRASS")) return Class_Grass;
			bool Tree = TreeProperty || NameStarts(VertexName, "ST") || NameStarts(PixelName, "ST");
			bool Skeleton = false, Block = false;
			NiAVObject* Object = Geometry;
			for (int Depth = 0; Object && Depth < 64; Depth++, Object = Object->m_parent) {
				if (Object == gRoots.LODTrees) return Class_LODTrees;
				if (Object == gRoots.DistantRef) return Class_DistantLOD;
				if (Object == gRoots.LandLOD) return Class_LandLOD;
				if (Object == gRoots.LODWater) return Class_Water;
				if (Object == gRoots.ObjectGrid) {
					if (SkinInstance || Skeleton) return Class_Actors;
					if (Block) return Class_Land;
					return Tree ? Class_Trees : Class_Statics;
				}
				if (gRoots.Shadow && Object->m_parent == gRoots.Shadow) { *Top = Object->m_pcName; break; }
				Skeleton |= NameStarts(Object->m_pcName, "Bip01");
				Block |= NameStarts(Object->m_pcName, "Block (");
			}
			if (Tree) return Class_Trees;
			if (SkinInstance || Skeleton) return Class_Actors;
			return Class_Other;
		}

		void LogTable(int Pass, double Inv, double TicksToMs) {
			Logger::Log("[FrameProfile]   -- draw census: %s (per frame; engine = nested SetupShaderPrograms CPU only) --", PassNames[Pass]);
			Logger::Log("[FrameProfile]   %-11s %7s %7s %7s %7s %7s %7s %7s %7s %5s %8s %7s %8s %8s %8s",
				"class", "draws", "maxDrw", "objects", "geoData", "mats", "lights", "mat+lt", "groups", "lit%", "engineMs", "us/draw", "drw/geo", "drw/grp", "drw/run");
			double Total[Col_COUNT] = {};
			LONGLONG TotalTicks = 0;
			for (int c = 0; c < Class_COUNT; c++) {
				const WindowRow& R = gWindow[Pass][c];
				for (int k = 0; k < Col_COUNT; k++) Total[k] += R.Sum[k];
				TotalTicks += R.EngineTicks;
				double Draws = R.Sum[Col_Draws] * Inv;
				if (Draws <= 0.0) continue;
				double Ms = R.EngineTicks * TicksToMs * Inv;
				Logger::Log("[FrameProfile]   %-11s %7.1f %7u %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f %5.0f %8.4f %7.2f %8.2f %8.2f %8.2f",
					ClassNames[c], Draws, R.MaxDraws, R.Sum[Col_Objects] * Inv, R.Sum[Col_GeomData] * Inv,
					R.Sum[Col_Materials] * Inv, R.Sum[Col_LightSets] * Inv, R.Sum[Col_MatLights] * Inv, R.Sum[Col_Groups] * Inv,
					100.0 * R.Sum[Col_Lit] / R.Sum[Col_Draws], Ms, Ms * 1000.0 / Draws,
					R.Sum[Col_GeomData] > 0.0 ? R.Sum[Col_Draws] / R.Sum[Col_GeomData] : 0.0,
					R.Sum[Col_Groups] > 0.0 ? R.Sum[Col_Draws] / R.Sum[Col_Groups] : 0.0,
					R.Sum[Col_Runs] > 0.0 ? R.Sum[Col_Draws] / R.Sum[Col_Runs] : 0.0);
			}
			double Draws = Total[Col_Draws] * Inv;
			double Ms = TotalTicks * TicksToMs * Inv;
			Logger::Log("[FrameProfile]   %-11s %7.1f %7s %7.1f %7.1f %7s %7s %7s %7.1f %5s %8.4f %7.2f   (sums of per-class distinct counts)",
				"total", Draws, "", Total[Col_Objects] * Inv, Total[Col_GeomData] * Inv, "", "", "", Total[Col_Groups] * Inv, "",
				Ms, Draws > 0.0 ? Ms * 1000.0 / Draws : 0.0);
		}

		int SizeBin(NiGeometry* Geometry) {
			NiCamera* Camera = WorldSceneGraph ? WorldSceneGraph->camera : NULL;
			NiBound* Bound = Geometry->GetWorldBound();
			if (!Camera || !Bound || !TheRenderManager) return SizeBinCount - 1;
			NiPoint2 Size = { 0.0f, 0.0f };
			TheRenderManager->GetScreenSpaceBoundSize(&Size, Bound, Camera);
			float Pixels = Size.y * (float)TheRenderManager->height;
			int Bin = 0;
			for (float Edge = 1.0f; Bin < SizeBinCount - 1 && Pixels >= Edge; Edge *= 2.0f) Bin++;
			return Bin;
		}

		void LogSizeBins(double Inv) {
			Logger::Log("[FrameProfile]   -- main-pass draws by bound diameter on screen (px of screen height; per frame) --");
			std::string Line = "[FrameProfile]   class      ";
			char Buffer[48];
			for (int b = 0; b < SizeBinCount; b++) {
				_snprintf_s(Buffer, _TRUNCATE, " %7s", SizeBinNames[b]);
				Line += Buffer;
			}
			Logger::Log("%s", Line.c_str());
			for (int c = 0; c < Class_COUNT; c++) {
				double Draws = 0.0;
				for (int b = 0; b < SizeBinCount; b++) Draws += gWindowSizeBins[c][b];
				if (Draws <= 0.0) continue;
				_snprintf_s(Buffer, _TRUNCATE, "[FrameProfile]   %-11s", ClassNames[c]);
				Line = Buffer;
				for (int b = 0; b < SizeBinCount; b++) {
					_snprintf_s(Buffer, _TRUNCATE, " %7.1f", gWindowSizeBins[c][b] * Inv);
					Line += Buffer;
				}
				Logger::Log("%s", Line.c_str());
			}
		}
	}

	int Record(void* Shader, NiGeometry* Geometry, NiSkinInstance* SkinInstance, NiPropertyState* PropertyState,
		NiD3DPass* ShaderPass, NiD3DVertexShaderEx* VertexShader, NiD3DPixelShaderEx* PixelShader) {

		if (!FrameProfiler::Active || !Geometry) return -1;
		int Pass = *kIsRenderingReflectionPass ? Pass_Reflection :
			TheShaderManager->InMainScenePass ? Pass_Main :
			RenderManager::CurrentPass == RenderManager::PassNear ? Pass_Shell : -1;
		if (Pass < 0) return -1;
		if (!gRootsValid) ResolveRoots();

		const char* VertexName = VertexShader ? VertexShader->ShaderName : NULL;
		const char* PixelName = PixelShader ? PixelShader->ShaderName : NULL;
		NiProperty* LightingProp = PropertyState ? PropertyState->prop[NiProperty::kType_Lighting] : NULL;
		PropInfo Info = LightingProp ? PropertyInfo(LightingProp) : PropInfo();

		const char* Top = NULL;
		int Class = Classify(Geometry, SkinInstance, VertexName, PixelName, Info.SpeedTree, &Top);
		if (Class == Class_Other) gOtherTops[Top ? Top : "(no anchor)"]++;
		if (Pass == Pass_Main) gFrameSizeBins[Class][SizeBin(Geometry)]++;

		Mix Material;
		Material.Add(Shader);
		Material.Add(VertexShader);
		Material.Add(PixelShader);
		Material.Add(LightingProp ? *(const void**)LightingProp : NULL);
		NiAlphaProperty* Alpha = PropertyState ? (NiAlphaProperty*)PropertyState->prop[NiProperty::kType_Alpha] : NULL;
		Material.Add(Alpha ? (UInt32)Alpha->flags | ((UInt32)Alpha->alphaTestRef << 16) : 0xFFFFFFFF);
		if (ShaderPass) {
			for (int i = 0; i < ShaderPass->Stages.end && i < 16; i++) {
				NiD3DTextureStage* Stage = ShaderPass->Stages.data[i];
				Material.Add(Stage ? Stage->Texture : NULL);
			}
		}
		UInt64 LightSet = Info.Lighting ? LightSetKey(LightingProp) : 0;

		Mix MatLight;
		MatLight.Add((UInt32)Material.H);
		MatLight.Add((UInt32)(Material.H >> 32));
		MatLight.Add((UInt32)LightSet);
		MatLight.Add((UInt32)(LightSet >> 32));
		Mix Group = MatLight;
		Group.Add(Geometry->geomData);

		FrameSlot& S = gFrame[Pass][Class];
		S.Draws++;
		if (Info.Lighting) S.Lit++;
		S.Objects.insert(Geometry);
		if (Geometry->geomData) S.GeomData.insert(Geometry->geomData);
		S.Materials.insert(Material.H);
		S.LightSets.insert(LightSet);
		S.MatLights.insert(MatLight.H);
		S.Groups.insert(Group.H);
		if (Group.H != gLastGroup[Pass]) S.Runs++;
		gLastGroup[Pass] = Group.H;
		return Pass * Class_COUNT + Class;
	}

	void AddEngineTicks(int Slot, LONGLONG Ticks) {
		gFrame[Slot / Class_COUNT][Slot % Class_COUNT].EngineTicks += Ticks;
	}

	void FrameEnd() {
		gRootsValid = false;
		for (int c = 0; c < Class_COUNT; c++) {
			for (int b = 0; b < SizeBinCount; b++) {
				gWindowSizeBins[c][b] += gFrameSizeBins[c][b];
				gFrameSizeBins[c][b] = 0;
			}
		}
		for (int p = 0; p < Pass_COUNT; p++) {
			gLastGroup[p] = 0;
			for (int c = 0; c < Class_COUNT; c++) {
				FrameSlot& S = gFrame[p][c];
				WindowRow& R = gWindow[p][c];
				R.Sum[Col_Draws] += S.Draws;
				R.Sum[Col_Objects] += (double)S.Objects.size();
				R.Sum[Col_GeomData] += (double)S.GeomData.size();
				R.Sum[Col_Materials] += (double)S.Materials.size();
				R.Sum[Col_LightSets] += (double)S.LightSets.size();
				R.Sum[Col_MatLights] += (double)S.MatLights.size();
				R.Sum[Col_Groups] += (double)S.Groups.size();
				R.Sum[Col_Runs] += S.Runs;
				R.Sum[Col_Lit] += S.Lit;
				R.EngineTicks += S.EngineTicks;
				if (S.Draws > R.MaxDraws) R.MaxDraws = S.Draws;
				S.Draws = S.Runs = S.Lit = 0;
				S.EngineTicks = 0;
				S.Objects.clear();
				S.GeomData.clear();
				S.Materials.clear();
				S.LightSets.clear();
				S.MatLights.clear();
				S.Groups.clear();
			}
		}
	}

	void Report(int Frames, LONGLONG QpcFreq) {
		double Recorded = 0.0;
		for (int p = 0; p < Pass_COUNT; p++)
			for (int c = 0; c < Class_COUNT; c++) Recorded += gWindow[p][c].Sum[Col_Draws];
		if (Recorded > 0.0 && Frames > 0 && QpcFreq > 0) {
			double Inv = 1.0 / Frames;
			double TicksToMs = 1000.0 / (double)QpcFreq;
			for (int p = 0; p < Pass_COUNT; p++) LogTable(p, Inv, TicksToMs);
			Logger::Log("[FrameProfile]   columns: geoData/mats/lights/mat+lt/groups are DISTINCT per frame; groups = (geometry data, material, light set),");
			Logger::Log("[FrameProfile]   the floor of instanced draws; runs = draws whose group differs from the previous draw of the pass; lit%% = draws with a light list.");
			LogSizeBins(Inv);
			if (!gOtherTops.empty()) {
				std::vector<std::pair<std::string, unsigned int>> Sorted(gOtherTops.begin(), gOtherTops.end());
				std::sort(Sorted.begin(), Sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				std::string Line;
				char Buffer[96];
				for (size_t i = 0; i < Sorted.size() && i < 8; i++) {
					_snprintf_s(Buffer, _TRUNCATE, "%s%.60s %.1f", i ? ", " : "", Sorted[i].first.c_str(), Sorted[i].second * Inv);
					Line += Buffer;
				}
				Logger::Log("[FrameProfile]   'other' draws by top node (per frame, all passes): %s", Line.c_str());
			}
		}
		for (int p = 0; p < Pass_COUNT; p++)
			for (int c = 0; c < Class_COUNT; c++) gWindow[p][c] = WindowRow();
		for (int c = 0; c < Class_COUNT; c++)
			for (int b = 0; b < SizeBinCount; b++) gWindowSizeBins[c][b] = 0.0;
		gOtherTops.clear();
	}
}
