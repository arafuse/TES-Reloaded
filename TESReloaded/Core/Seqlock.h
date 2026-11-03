#pragma once

#include <intrin.h>

/// A snapshot one thread writes and other threads copy out without locking. The sequence is odd while
/// the writer copies; a reader keeps its copy only if it saw the same even sequence before and after.
template <typename T>
class Seqlock {
public:
	/// Replaces the snapshot. Call from one writer thread only.
	void Publish(const T& Next) {
		InterlockedIncrement(&Sequence);
		Value = Next;
		InterlockedIncrement(&Sequence);
	}

	/// Copies the snapshot into Out. Returns false when writes kept overlapping the copy.
	bool Read(T& Out) const {
		for (int Try = 0; Try < kReadTries; Try++) {
			LONG Before = Sequence;
			_ReadWriteBarrier();
			if (Before & 1) {
				YieldProcessor();
				continue;
			}
			Out = Value;
			_ReadWriteBarrier();
			if (Sequence == Before) return true;
		}
		return false;
	}

private:
	static const int	kReadTries = 4;
	volatile LONG		Sequence = 0;
	T					Value = {};
};
