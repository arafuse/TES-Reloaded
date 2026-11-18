#pragma once

/// Smooths actor animation transitions ([AnimationBlending] in OblivionReloaded.ini). Oblivion only.
///
/// Starting a sequence in a slot (ActorAnimData 0x474530) already crossfades or blends from the current
/// pose, but for only max(KF "Blend:" key / 30, fAnimationDefaultBlend = 0.1 s). That time is raised to
/// MinBlendTime, or ActionMinBlendTime when the new group is an attack, block, recoil, cast, dodge, jump,
/// equip or death. Stopping a single slot (0x470FC0) almost always passes a fade of 0, which snaps the
/// bones back to the slot underneath; those stops ease out over StopBlendTime instead.
///
/// Transitions the engine makes instant on purpose are left alone: the ActorAnimData +0xC4 no-blend flag
/// (resets, AI package changes, knockdown), stops of every slot at once, and the stops 0x474530 makes
/// itself before blending from the pose.
void CreateAnimationBlendingHook();
