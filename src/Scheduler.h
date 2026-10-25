#pragma once
// Which objects get an occlusion query this frame, in what order. Engine-free, shared with tests\sched_test.cpp.
//
// Who may be tested:
//   - covered objects (hidden now, or their last answer was 0 pixels): every frame; they must stay fresh, or they are
//     drawn again after iMaxAnswerAge frames, and an object coming back into view must be seen soon
//   - objects never tested yet: always
//   - visible objects: only once every iVisibleRetestFrames frames. Drawing a visible object is always right, so a
//     skipped test can only delay hiding it, never hide anything wrongly.
// The budget (iMaxQueriesPerFrame) goes to the longest-waiting first, with a covered object's wait counting double,
// and never-tested objects first of all. A strict class order starved the rest: with more covered objects than the
// budget, the same ones won every frame and objects never tested stayed untested (simulation: 40% of the Old Mormon
// Fort's covered objects hidden instead of all of them).

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Scheduler {

	enum Class : uint8_t { kCovered = 0, kNew = 1, kVisible = 2, kNotDue = 255 };

	inline Class Classify(bool answered, uint8_t zeroStreak, uint32_t sinceLastTest, uint32_t visibleInterval) {
		if (!answered) return kNew;
		if (zeroStreak > 0) return kCovered;
		return sinceLastTest >= visibleInterval ? kVisible : kNotDue;
	}

	struct Item {
		uint8_t cls;
		uint32_t sinceLastTest;
		uint32_t index;   // position in the candidate list (keeps the order stable)
	};

	inline uint64_t Urgency(const Item& item) {
		if (item.cls == kNew) return ~0ull;
		return (uint64_t)item.sinceLastTest * (item.cls == kCovered ? 2u : 1u);
	}

	inline void Order(std::vector<Item>& items) {
		std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
			const uint64_t ua = Urgency(a), ub = Urgency(b);
			if (ua != ub) return ua > ub;
			if (a.cls != b.cls) return a.cls < b.cls;
			return a.index < b.index;
		});
	}
}
