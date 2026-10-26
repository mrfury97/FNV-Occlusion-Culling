// Simulates the plugin's per-frame loop with Scheduler.h: answers arrive one frame after the test, an object is hidden
// when its last two answers were 0 pixels and the newest is at most maxAge frames old (camera guard left out).
#include <cstdio>
#include <vector>
#include "../src/Scheduler.h"

static int gFails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++gFails; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Obj { bool covered; bool answered = false, pending = false; uint8_t zero = 0; uint32_t issued = 0, answerFrame = 0, pendingTruth = 0; };

struct Result { double hiddenShare, visibleWronglyHidden, maxSinceCoveredTest, testsPerFrame; };

// coveredAt(i, frame): is object i covered in that frame
template <typename F>
static Result Run(int n, int budget, int maxAge, int interval, int frames, F coveredAt) {
	std::vector<Obj> o(n);
	uint32_t maxSince = 0;
	double hidden = 0, coveredFrames = 0, wrong = 0, tests = 0;
	for (uint32_t frame = 1; frame <= (uint32_t)frames; ++frame) {
		for (int i = 0; i < n; ++i) o[i].covered = coveredAt(i, frame);
		// answers from the last frame's tests
		for (auto& x : o)
			if (x.pending) { x.pending = false; x.answered = true; x.answerFrame = x.issued; x.zero = x.pendingTruth ? 0 : (uint8_t)(x.zero + 1); }
		// hide decisions (as Collect)
		for (auto& x : o) {
			const bool hide = x.answered && x.zero >= 2 && frame - x.answerFrame >= 1 && frame - x.answerFrame <= (uint32_t)maxAge;
			if (frame > 50) {
				if (x.covered) { coveredFrames += 1; if (hide) hidden += 1; }
				else if (hide) wrong += 1;
			}
		}
		// schedule and issue (as RunQueries)
		std::vector<Scheduler::Item> order;
		for (uint32_t i = 0; i < (uint32_t)n; ++i) {
			const Obj& x = o[i];
			if (x.pending) continue;
			const auto cls = Scheduler::Classify(x.answered, x.zero, frame - x.issued, (uint32_t)interval);
			if (cls == Scheduler::kNotDue) continue;
			order.push_back({ (uint8_t)cls, frame - x.issued, i });
		}
		Scheduler::Order(order);
		int issued = 0;
		for (const auto& it : order) {
			if (issued >= budget) break;
			Obj& x = o[it.index];
			x.pending = true; x.issued = frame; x.pendingTruth = x.covered ? 0 : 1; ++issued;
		}
		tests += issued;
		if (frame > 50)
			for (auto& x : o) if (x.covered && x.zero > 0 && frame - x.issued > maxSince) maxSince = frame - x.issued;
	}
	const double measured = frames - 50;
	return { coveredFrames > 0 ? hidden / coveredFrames : 1.0, wrong / measured, (double)maxSince, tests / frames };
}

int main() {
	// Old Mormon Fort: 1300 candidates, 95% covered, budget 500.
	auto fort = Run(1300, 500, 4, 4, 600, [](int i, uint32_t) { return i % 20 != 0; });
	printf("fort (1300, 95%% covered, budget 500): %.1f%% of covered objects hidden, %.2f visible ones wrongly hidden per frame, "
		"longest gap between tests of a covered object %.0f frames, %.0f tests per frame\n",
		fort.hiddenShare * 100, fort.visibleWronglyHidden, fort.maxSinceCoveredTest, fort.testsPerFrame);
	CHECK(fort.hiddenShare > 0.95, "fort: only %.1f%% hidden", fort.hiddenShare * 100);
	CHECK(fort.visibleWronglyHidden == 0, "fort: visible objects hidden");
	CHECK(fort.maxSinceCoveredTest <= 3, "fort: a covered object waited %.0f frames", fort.maxSinceCoveredTest);

	// Open street: 800 candidates, 15% covered.
	auto street = Run(800, 500, 4, 4, 600, [](int i, uint32_t) { return i % 7 == 0; });
	printf("street (800, 15%% covered): %.1f%% hidden, %.2f wrongly hidden, %.0f tests per frame\n", street.hiddenShare * 100,
		street.visibleWronglyHidden, street.testsPerFrame);
	CHECK(street.hiddenShare > 0.95 && street.visibleWronglyHidden == 0, "street");
	CHECK(street.testsPerFrame < 320, "street: visible objects retested too often (%.0f per frame)", street.testsPerFrame);

	// Turning: every 30 frames a different half is covered. Objects coming into view must not stay hidden.
	auto turning = Run(1300, 500, 4, 4, 900, [](int i, uint32_t f) { return ((i / 10) + f / 30) % 2 == 0; });
	printf("turning (1300, half swaps every 30 frames): %.1f%% of covered hidden, %.2f wrongly hidden per frame\n",
		turning.hiddenShare * 100, turning.visibleWronglyHidden);
	// An object that just came into view keeps its old 0-pixel answers until its next test (one frame here, at the top of
	// the queue): at most one frame of 650 objects per switch, never longer.
	CHECK(turning.visibleWronglyHidden <= 650.0 * 2 / 30, "turning: %.1f wrongly hidden per frame", turning.visibleWronglyHidden);

	// Over budget: 3000 candidates all covered, budget 500 -> covered objects can only be retested every 6 frames, so the
	// answers age past 4 and part of them are drawn; nothing visible is ever hidden.
	auto huge = Run(3000, 500, 4, 4, 600, [](int, uint32_t) { return true; });
	printf("huge (3000 covered, budget 500): %.1f%% hidden, longest gap %.0f frames\n", huge.hiddenShare * 100, huge.maxSinceCoveredTest);
	CHECK(huge.visibleWronglyHidden == 0, "huge: wrongly hidden");

	// SelectTop picks exactly the items the first n of Order would (many equal urgencies, all classes).
	{
		bool same = true;
		uint32_t seed = 12345;
		auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
		for (int trial = 0; trial < 200 && same; ++trial) {
			std::vector<Scheduler::Item> a;
			const uint32_t count = 50 + rnd() % 3000;
			for (uint32_t i = 0; i < count; ++i) a.push_back({ (uint8_t)(rnd() % 3), rnd() % 8, i });
			std::vector<Scheduler::Item> b = a;
			const size_t n = rnd() % (count + 10);
			Scheduler::Order(a);
			Scheduler::SelectTop(b, n);
			std::vector<char> inA(count, 0), inB(count, 0);
			for (size_t k = 0; k < n && k < count; ++k) { inA[a[k].index] = 1; inB[b[k].index] = 1; }
			same = inA == inB;
		}
		CHECK(same, "SelectTop chose different items than Order");
	}

	printf(gFails ? "%d FAILED\n" : "all scheduler checks passed\n", gFails);
	return gFails ? 1 : 0;
}
