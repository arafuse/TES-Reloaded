#include "RagdollCollision.h"

static const UInt32 kInitLayerMatrixCall	= 0x0088B157; // Call site in the Havok setup run by the TES constructor
static const UInt32 kInitLayerMatrix		= 0x008A83C0; // Builds the layer collision matrix at 0x00BA7DB0
static const UInt32 kSetLayerCollision		= 0x008A7F20; // Sets or clears a layer pair, both directions

static const UInt32 kLayerBiped				= 8;
static const UInt32 kLayerCharController	= 20;

static void InitLayerMatrixHook() {

	((void (__cdecl*)())kInitLayerMatrix)();
	// The engine disables this pair on purpose; runtime layer toggles only touch the CustomPick layers.
	((void (__cdecl*)(UInt32, UInt32, bool))kSetLayerCollision)(kLayerBiped, kLayerCharController, true);

}

void CreateRagdollCollisionHook() {

	WriteRelCall(kInitLayerMatrixCall, (UInt32)InitLayerMatrixHook);

}
