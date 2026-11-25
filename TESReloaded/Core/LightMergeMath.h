#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <vector>

/// Engine-free clustering for the light merge (LightMerge.cpp): which matching lights form a cluster,
/// how each fades into or out of it, and where the cluster's leader light sits, how far it reaches
/// and how bright it is. Proven by Tools/Tests/LightMergeMathTest.cpp.
namespace LightMergeMath {

	/// Tuning, from the [LightMerge] INI section.
	struct Params {
		float JoinDistance, LeaveDistance, FadeTime, BrightnessExponent, ColorTolerance, RadiusTolerance;
	};

	/// One scene light this frame. Key identifies it across frames; Eligible is false for lights that
	/// must never merge (the player's light, magic lights, non-carried lights).
	struct LightInput {
		const void* Key;
		float       Pos[3];
		float       Color[3];
		float       Radius;
		bool        Eligible;
	};

	enum Role { Role_Solo, Role_Leader, Role_Follower };

	/// What to do with one light this frame. ColorScale multiplies its diffuse color; a leader's Pos
	/// and Radius replace its own; a Hidden follower is fully absorbed and may be removed from the
	/// engine's light assignment.
	struct LightOutput {
		const void* Key;
		Role        Role;
		float       Weight;
		float       ColorScale;
		float       Pos[3];
		float       Radius;
		bool        Hidden;
		const void* LeaderKey;
	};

	inline float Distance(const float A[3], const float B[3]) {
		float X = A[0] - B[0], Y = A[1] - B[1], Z = A[2] - B[2];
		return std::sqrt(X * X + Y * Y + Z * Z);
	}

	/// True when two lights are the same light for merging: every color channel and the radius within tolerance.
	inline bool Matches(const LightInput& A, const LightInput& B, const Params& P) {
		for (int i = 0; i < 3; i++)
			if (std::fabs(A.Color[i] - B.Color[i]) > P.ColorTolerance) return false;
		return std::fabs(A.Radius - B.Radius) <= P.RadiusTolerance;
	}

	/// The leader's brightness factor: (1 + sum of follower weights)^Exponent.
	inline float LeaderScale(float FollowerWeightSum, float Exponent) {
		return std::pow(1.0f + FollowerWeightSum, Exponent);
	}

	/// True when merge state must start over: the first frame, a different worldspace or interior
	/// (Scene is the worldspace, or the cell when interior), or a gap long enough to be a load. An
	/// exterior cell border is not a break.
	inline bool IsSceneBreak(const void* PrevScene, const void* Scene, double GapMs, double LoadGapMs) {
		return !PrevScene || PrevScene != Scene || GapMs > LoadGapMs;
	}

	/// One merged light's real values, as the actor light level must see them while the render has
	/// the light moved, dimmed or hidden. Source is the engine light the level code excludes by.
	struct TrueLight {
		const void* Key;
		const void* Source;
		float       Pos[3];
		float       Radius;
		float       MaxDiffuse;
		bool        Hidden;
	};

	/// The engine's light-level share (0x7D31B0): (1 - clamp(d/r)^2) * max diffuse; 0 for the
	/// excluded light (the actor's own).
	inline float LightLevelShare(const TrueLight& L, const float At[3], const void* Exclude) {
		if (L.Source == Exclude || L.Radius <= 0.0f) return 0.0f;
		float T = Distance(L.Pos, At) / L.Radius;
		if (T > 1.0f) T = 1.0f;
		return (1.0f - T * T) * L.MaxDiffuse;
	}

	/// Double-buffered table of TrueLights: one writer (the main thread, once a frame) and readers on
	/// any thread. A View taken before a Publish stays intact through the next one.
	class TrueLightTable {
	public:
		static const int Capacity = 64;

		struct View {
			const TrueLight* Lights;
			int              Count;
			const TrueLight* Find(const void* Key) const {
				for (int i = 0; i < Count; i++)
					if (Lights[i].Key == Key) return &Lights[i];
				return nullptr;
			}
		};

		/// The buffer readers are not using; fill up to Capacity entries, then Publish.
		TrueLight* BeginWrite() { return Buffers[1 - Front.load(std::memory_order_acquire)]; }

		void Publish(int Count) {
			int Back = 1 - Front.load(std::memory_order_relaxed);
			Counts[Back] = Count < Capacity ? Count : Capacity;
			Front.store(Back, std::memory_order_release);
		}

		View Read() const {
			int F = Front.load(std::memory_order_acquire);
			View V = { Buffers[F], Counts[F] };
			return V;
		}

	private:
		TrueLight        Buffers[2][Capacity] = {};
		int              Counts[2] = { 0, 0 };
		std::atomic<int> Front{ 0 };
	};

	/// Keeps clusters stable across frames: hysteresis between JoinDistance and LeaveDistance, fade
	/// weights stepped by elapsed time, and a leader kept for the cluster's whole life.
	class Clusterer {
	public:
		void Reset() { Clusters.clear(); Outputs_.clear(); }

		/// Snap = true sets every weight to its target (load, cell change, FadeTime <= 0).
		void Update(const LightInput* Lights, int Count, const Params& P, float DeltaSeconds, bool Snap) {
			Index.clear();
			for (int i = 0; i < Count; i++) Index[Lights[i].Key] = i;
			Assigned.assign(Count, 0);

			UpdateExistingClusters(Lights, P);
			JoinExistingClusters(Lights, Count, P);
			FormNewClusters(Lights, Count, P);
			StepWeights(P, DeltaSeconds, Snap);
			WriteOutputs(Lights, Count, P);
		}

		/// One per input of the last Update, same order.
		const std::vector<LightOutput>& Outputs() const { return Outputs_; }

	private:
		struct Member { const void* Key; float Weight; float Target; };
		struct Cluster {
			const void*         Leader;
			std::vector<Member> Followers;
			float               Centroid[3];
			bool                Dissolving;
		};

		std::vector<Cluster>                  Clusters;
		std::vector<LightOutput>              Outputs_;
		std::unordered_map<const void*, int>  Index;
		std::vector<char>                     Assigned;

		int Find(const void* Key) const {
			auto It = Index.find(Key);
			return It == Index.end() ? -1 : It->second;
		}

		void UpdateExistingClusters(const LightInput* Lights, const Params& P) {
			for (size_t c = 0; c < Clusters.size(); ) {
				Cluster& C = Clusters[c];
				for (size_t f = 0; f < C.Followers.size(); ) {
					if (Find(C.Followers[f].Key) < 0) C.Followers.erase(C.Followers.begin() + f);
					else f++;
				}
				int Leader = Find(C.Leader);
				if ((Leader < 0 || !Lights[Leader].Eligible) && !PromoteFollower(C, Lights)) {
					Clusters.erase(Clusters.begin() + c);
					continue;
				}
				Leader = Find(C.Leader);
				Assigned[Leader] = 1;
				if (C.Followers.empty()) {
					Clusters.erase(Clusters.begin() + c);
					continue;
				}
				if (!C.Dissolving) C.Dissolving = LeaderLeft(C, Lights[Leader], P);
				for (Member& M : C.Followers) {
					int i = Find(M.Key);
					Assigned[i] = 1;
					const LightInput& L = Lights[i];
					bool Valid = L.Eligible && Matches(Lights[Leader], L, P);
					float Dist = Distance(L.Pos, C.Centroid);
					if (C.Dissolving || !Valid) M.Target = 0.0f;
					else if (M.Target > 0.0f) M.Target = Dist <= P.LeaveDistance ? 1.0f : 0.0f;
					else M.Target = Dist <= P.JoinDistance ? 1.0f : 0.0f;
				}
				c++;
			}
		}

		/// The leader's light is gone or ineligible: the most absorbed eligible follower (nearest the
		/// centre on a tie) leads instead, so the others stay merged and the group's brightness does not
		/// jump. False when no follower is left to lead.
		bool PromoteFollower(Cluster& C, const LightInput* Lights) {
			int Best = -1;
			float BestDist = 0.0f;
			for (size_t f = 0; f < C.Followers.size(); f++) {
				const LightInput& L = Lights[Find(C.Followers[f].Key)];
				if (!L.Eligible) continue;
				float Dist = Distance(L.Pos, C.Centroid);
				float W = C.Followers[f].Weight, BestW = Best < 0 ? -1.0f : C.Followers[Best].Weight;
				if (W > BestW || (W == BestW && Dist < BestDist)) { Best = (int)f; BestDist = Dist; }
			}
			if (Best < 0) return false;
			C.Leader = C.Followers[Best].Key;
			C.Followers.erase(C.Followers.begin() + Best);
			return !C.Followers.empty();
		}

		/// The leader has walked beyond LeaveDistance of the cluster's last centre (the same test a
		/// follower leaves by, so a two-light cluster behaves the same whichever light walks off).
		static bool LeaderLeft(const Cluster& C, const LightInput& Leader, const Params& P) {
			return Distance(Leader.Pos, C.Centroid) > P.LeaveDistance;
		}

		void JoinExistingClusters(const LightInput* Lights, int Count, const Params& P) {
			for (int i = 0; i < Count; i++) {
				if (Assigned[i] || !Lights[i].Eligible) continue;
				Cluster* Best = nullptr;
				float BestDist = P.JoinDistance;
				for (Cluster& C : Clusters) {
					if (C.Dissolving || !Matches(Lights[Find(C.Leader)], Lights[i], P)) continue;
					float Dist = Distance(Lights[i].Pos, C.Centroid);
					if (Dist <= BestDist) { BestDist = Dist; Best = &C; }
				}
				if (!Best) continue;
				Best->Followers.push_back({ Lights[i].Key, 0.0f, 1.0f });
				Assigned[i] = 1;
			}
		}

		void FormNewClusters(const LightInput* Lights, int Count, const Params& P) {
			std::vector<int> Group;
			for (int i = 0; i < Count; i++) {
				if (Assigned[i] || !Lights[i].Eligible) continue;
				Group.assign(1, i);
				for (int j = 0; j < Count; j++) {
					if (j == i || Assigned[j] || !Lights[j].Eligible || !Matches(Lights[i], Lights[j], P)) continue;
					if (Distance(Lights[i].Pos, Lights[j].Pos) <= P.JoinDistance) Group.push_back(j);
				}
				if (Group.size() < 2) continue;
				std::sort(Group.begin(), Group.end());
				float Center[3] = { 0.0f, 0.0f, 0.0f };
				for (int g : Group)
					for (int k = 0; k < 3; k++) Center[k] += Lights[g].Pos[k] / (float)Group.size();
				int Leader = Group[0];
				float LeaderDist = Distance(Lights[Leader].Pos, Center);
				for (int g : Group) {
					float Dist = Distance(Lights[g].Pos, Center);
					if (Dist < LeaderDist) { LeaderDist = Dist; Leader = g; }
				}
				Cluster C = {};
				C.Leader = Lights[Leader].Key;
				for (int g : Group) {
					Assigned[g] = 1;
					if (g != Leader) C.Followers.push_back({ Lights[g].Key, 0.0f, 1.0f });
				}
				Clusters.push_back(C);
			}
		}

		void StepWeights(const Params& P, float DeltaSeconds, bool Snap) {
			bool Instant = Snap || P.FadeTime <= 0.0f;
			float Step = Instant ? 1.0f : DeltaSeconds / P.FadeTime;
			for (size_t c = 0; c < Clusters.size(); ) {
				std::vector<Member>& F = Clusters[c].Followers;
				for (size_t f = 0; f < F.size(); ) {
					Member& M = F[f];
					if (M.Weight < M.Target) M.Weight = std::fmin(M.Weight + Step, M.Target);
					else if (M.Weight > M.Target) M.Weight = std::fmax(M.Weight - Step, M.Target);
					if (M.Target <= 0.0f && M.Weight <= 0.0f) F.erase(F.begin() + f);
					else f++;
				}
				if (F.empty()) Clusters.erase(Clusters.begin() + c);
				else c++;
			}
		}

		void WriteOutputs(const LightInput* Lights, int Count, const Params& P) {
			Outputs_.resize(Count);
			for (int i = 0; i < Count; i++) {
				LightOutput& O = Outputs_[i];
				O.Key = Lights[i].Key;
				O.Role = Role_Solo;
				O.Weight = 0.0f;
				O.ColorScale = 1.0f;
				for (int k = 0; k < 3; k++) O.Pos[k] = Lights[i].Pos[k];
				O.Radius = Lights[i].Radius;
				O.Hidden = false;
				O.LeaderKey = nullptr;
			}
			for (Cluster& C : Clusters) {
				const LightInput& Leader = Lights[Find(C.Leader)];
				float Sum[3] = { Leader.Pos[0], Leader.Pos[1], Leader.Pos[2] };
				float WeightSum = 0.0f;
				for (const Member& M : C.Followers) {
					const LightInput& L = Lights[Find(M.Key)];
					for (int k = 0; k < 3; k++) Sum[k] += M.Weight * L.Pos[k];
					WeightSum += M.Weight;
				}
				for (int k = 0; k < 3; k++) C.Centroid[k] = Sum[k] / (1.0f + WeightSum);
				float Reach = Distance(Leader.Pos, C.Centroid);
				for (const Member& M : C.Followers) {
					int i = Find(M.Key);
					Reach = std::fmax(Reach, M.Weight * Distance(Lights[i].Pos, C.Centroid));
					LightOutput& O = Outputs_[i];
					O.Role = Role_Follower;
					O.Weight = M.Weight;
					O.ColorScale = 1.0f - M.Weight;
					O.Hidden = M.Weight >= 1.0f;
					O.LeaderKey = C.Leader;
				}
				LightOutput& O = Outputs_[Find(C.Leader)];
				O.Role = Role_Leader;
				O.ColorScale = LeaderScale(WeightSum, P.BrightnessExponent);
				for (int k = 0; k < 3; k++) O.Pos[k] = C.Centroid[k];
				O.Radius = Leader.Radius + Reach;
				O.LeaderKey = C.Leader;
			}
		}
	};

}
