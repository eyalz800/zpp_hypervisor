/**
 * `zpp::small_map`, the fixed-capacity sorted map the hypervisor keeps
 * `module_physical_to_virtual` in.
 *
 * It had no test of any kind. It is a header with no target dependency -
 * `<algorithm>`, `<functional>`, `<new>` and nothing else - so it
 * compiles here exactly as the hypervisor compiles it, which is the same
 * argument tests/elf_relocate and tests/mtrr make for themselves.
 *
 * What it pins, and the reason this file exists: `end()` computed its
 * pointer as `std::addressof(value(m_size - 1)) + 1`. On an empty map
 * `m_size - 1` is unsigned and wraps to SIZE_MAX, so the expression
 * formed a reference one element *before* the storage array and then
 * walked the pointer back. Same pointer, undefined behaviour, and
 * invisible for as long as nobody looked - it survived every run of the
 * hosted suite on three platforms and was found by
 * UndefinedBehaviorSanitizer, not by a failure.
 *
 * The empty case is the whole of it, so most of what follows is about a
 * map with nothing in it. The rest is the ordinary contract, asserted
 * because a container nothing tests is a container whose next edit is
 * unguarded - the class comment itself says the copy, move and
 * assignment members "have never been compiled by anything here".
 */
#include "zpp/small_map.h"

#include <cstddef>
#include <cstdint>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::size_t g_checks{};
std::size_t g_failures{};

void check(bool condition, const std::string & what)
{
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::println("FAIL: {}", what);
}

void check_equal(std::uint64_t expected,
                 std::uint64_t actual,
                 const std::string & what)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::println(
        "FAIL: {}\n  expected {}\n  actual   {}", what, expected, actual);
}

/**
 * The instantiation under test.
 *
 * Key and value are both integral, which is what the hypervisor's own
 * instantiation uses - `module_physical_to_virtual` maps a physical
 * address to a virtual one - so the static_asserts at the top of the
 * class are satisfied the same way.
 */
using map = zpp::small_map<std::uint64_t, std::uint64_t, 8>;

// === The empty map, which is where the defect was =====================

/**
 * `end()` on an empty map is `begin()`, and is arrived at without
 * forming a pointer outside the storage.
 *
 * The equality is what every caller depends on. The second check is the
 * regression itself: the old spelling produced the *same* pointer, so
 * equality alone would have passed throughout - what it did wrong was
 * compute it by going one element below the array and back. There is no
 * portable way to observe that from inside the language, which is
 * exactly why this went unnoticed; what a test can do is pin the
 * relationship that makes the wrapping spelling impossible to
 * reintroduce accidentally.
 */
void an_empty_map_ends_where_it_begins()
{
    map empty;

    check(empty.begin() == empty.end(),
          "an empty map's begin() and end() are the same pointer");
    check_equal(
        0,
        static_cast<std::uint64_t>(empty.end() - empty.begin()),
        "and the distance between them is zero, which is what "
        "`begin() + m_size` gives and what `value(m_size - 1) + 1` "
        "gave only by wrapping through SIZE_MAX first");
    check(empty.cbegin() == empty.cend(), "the const iterators agree");

    const map & constant = empty;
    check(constant.begin() == constant.end(),
          "and so does the const overload reached through a reference");
}

/**
 * An empty map reports empty, and iterating one visits nothing.
 *
 * The range-for is not decoration: it is the shape that calls `end()`
 * on an empty container, which is the call that was undefined.
 */
void an_empty_map_iterates_over_nothing()
{
    map empty;

    check(empty.empty(), "an empty map says it is empty");
    check_equal(0, empty.size(), "and its size is zero");

    std::size_t visited{};
    for (const auto & entry : empty) {
        static_cast<void>(entry);
        ++visited;
    }
    check_equal(0, visited, "a range-for over it visits nothing");

    check(empty.find(0) == empty.end(),
          "find on an empty map answers end()");
    check(empty.find(0xdeadbeef) == empty.end(), "for any key at all");
}

/**
 * Emptied by `clear()` and by erasing every element, the map is the same
 * empty map again.
 *
 * Two ways back to zero, because `clear()` assigns `m_size` and `erase`
 * decrements it, and only one of them was on the path the defect was
 * found through.
 */
void a_map_emptied_again_still_ends_where_it_begins()
{
    map cleared{{1, 10}, {2, 20}, {3, 30}};
    cleared.clear();
    check_equal(0, cleared.size(), "clear() empties the map");
    check(cleared.begin() == cleared.end(),
          "and end() is begin() again afterwards");

    map erased{{1, 10}, {2, 20}, {3, 30}};
    while (!erased.empty()) {
        erased.erase(erased.begin());
    }
    check_equal(0, erased.size(), "erasing one at a time empties it");
    check(erased.begin() == erased.end(),
          "and end() is begin() again afterwards");

    map ranged{{1, 10}, {2, 20}, {3, 30}};
    ranged.erase(ranged.begin(), ranged.end());
    check_equal(0, ranged.size(), "erasing the whole range empties it");
    check(ranged.begin() == ranged.end(),
          "and end() is begin() again afterwards");
}

// === The ordinary contract ============================================

/**
 * Keys come out in order however they went in, which is what makes the
 * binary search in `find_index` correct.
 */
void the_map_is_sorted_by_key()
{
    map built;
    for (auto key : {5ull, 1ull, 4ull, 2ull, 3ull}) {
        built.insert({key, key * 10});
    }

    check_equal(5, built.size(), "five distinct keys give a size of five");

    std::vector<std::uint64_t> seen;
    for (const auto & entry : built) {
        seen.push_back(entry.first);
    }

    check(seen == std::vector<std::uint64_t>{1, 2, 3, 4, 5},
          "iteration is in ascending key order whatever the insert order");
    check_equal(1, built.front().first, "front() is the smallest key");
    check_equal(5, built.back().first, "back() is the largest");
}

/**
 * Every key that went in is found, and one that did not is not.
 */
void every_key_is_found_and_no_others()
{
    map built{{2, 20}, {4, 40}, {6, 60}};

    for (auto key : {2ull, 4ull, 6ull}) {
        auto found = built.find(key);
        check(found != built.end(),
              "a key that was inserted is found: " + std::to_string(key));
        if (found != built.end()) {
            check_equal(
                key * 10, found->second, "and carries its own value");
        }
    }

    for (auto key : {0ull, 1ull, 3ull, 5ull, 7ull, 100ull}) {
        check(built.find(key) == built.end(),
              "a key that was not inserted answers end(): " +
                  std::to_string(key));
    }
}

/**
 * Inserting a key twice replaces rather than duplicates.
 *
 * A sorted array with two equal keys breaks the binary search below it,
 * so which of the two behaviours this has is load bearing rather than a
 * preference.
 */
void a_repeated_key_replaces_its_value()
{
    map built{{1, 10}, {2, 20}};
    built.insert({1, 999});

    check_equal(2, built.size(), "the size does not grow");
    auto found = built.find(1);
    check(found != built.end(), "the key is still there");
    if (found != built.end()) {
        check_equal(999, found->second, "carrying the newer value");
    }
}

/**
 * Erasing one element removes that one and shifts the rest down.
 */
void erasing_removes_exactly_one()
{
    map built{{1, 10}, {2, 20}, {3, 30}, {4, 40}};

    built.erase(built.find(2));

    check_equal(3, built.size(), "the size drops by one");
    check(built.find(2) == built.end(), "the erased key is gone");

    std::vector<std::uint64_t> seen;
    for (const auto & entry : built) {
        seen.push_back(entry.first);
    }
    check(seen == std::vector<std::uint64_t>{1, 3, 4},
          "and the survivors are still in order with no hole");
}

/**
 * Erasing a range removes exactly that range.
 */
void erasing_a_range_removes_exactly_it()
{
    map built{{1, 10}, {2, 20}, {3, 30}, {4, 40}, {5, 50}};

    built.erase(built.begin() + 1, built.begin() + 3);

    check_equal(3, built.size(), "the size drops by the range's length");

    std::vector<std::uint64_t> seen;
    for (const auto & entry : built) {
        seen.push_back(entry.first);
    }
    check(seen == std::vector<std::uint64_t>{1, 4, 5},
          "and the two named keys are the two that went");
}

/**
 * An empty range erases nothing, which is the other place `end()` is
 * evaluated against itself.
 */
void erasing_an_empty_range_does_nothing()
{
    map built{{1, 10}, {2, 20}};

    built.erase(built.end(), built.end());
    check_equal(2, built.size(), "erasing [end, end) leaves the size");

    built.erase(built.begin(), built.begin());
    check_equal(2, built.size(), "and so does erasing [begin, begin)");

    map empty;
    empty.erase(empty.begin(), empty.end());
    check_equal(0,
                empty.size(),
                "erasing the whole of an empty map is a no-op rather "
                "than a wrap");
}

/**
 * `emplace` builds the value in place and answers the same questions
 * `insert` does.
 */
void emplace_puts_the_key_where_insert_would()
{
    map built;
    built.emplace(3ull, 30ull);
    built.emplace(1ull, 10ull);
    built.emplace(2ull, 20ull);

    check_equal(3, built.size(), "three emplaces give three entries");

    std::vector<std::uint64_t> seen;
    for (const auto & entry : built) {
        seen.push_back(entry.first);
    }
    check(seen == std::vector<std::uint64_t>{1, 2, 3},
          "in ascending key order");
}

/**
 * The map holds its whole declared capacity.
 *
 * Bounding `m_size` against `Size` is the caller's job - the class says
 * so - so what is asserted here is only that filling it exactly does not
 * misbehave, not that overfilling is caught.
 */
void the_map_holds_its_capacity()
{
    map built;
    for (std::uint64_t key{}; key < 8; ++key) {
        built.insert({key, key * 10});
    }

    check_equal(8, built.size(), "eight entries fit in a capacity of 8");
    check_equal(8,
                static_cast<std::uint64_t>(built.end() - built.begin()),
                "and end() - begin() is the size at full capacity too");
    check_equal(0, built.front().first, "front is the smallest");
    check_equal(7, built.back().first, "back is the largest");

    for (std::uint64_t key{}; key < 8; ++key) {
        check(built.find(key) != built.end(),
              "every one of them is findable: " + std::to_string(key));
    }
}

/**
 * A moved-from map is left empty, and an empty one is what the previous
 * cases describe.
 *
 * The class comment says the copy, move and assignment members "have
 * never been compiled by anything here", which is a statement about
 * coverage rather than about correctness. Compiling them is most of the
 * value of this case.
 */
void a_moved_from_map_is_empty()
{
    map source{{1, 10}, {2, 20}, {3, 30}};
    map moved{std::move(source)};

    check_equal(3, moved.size(), "the move takes the entries");
    check_equal(0, source.size(), "and leaves the source empty");
    check(source.begin() == source.end(),
          "so the source ends where it begins, like any empty map");

    map assigned;
    assigned = std::move(moved);
    check_equal(3, assigned.size(), "move assignment takes them too");
    check_equal(0, moved.size(), "and empties its source");
    check(moved.begin() == moved.end(),
          "which is again an ordinary empty map");
}

/**
 * A copy is independent of its original.
 */
void a_copy_is_independent()
{
    map source{{1, 10}, {2, 20}};
    map copy{source};

    check_equal(2, copy.size(), "the copy has the same entries");

    copy.insert({3, 30});
    check_equal(3, copy.size(), "and can grow");
    check_equal(2, source.size(), "without the original growing");
    check(source.find(3) == source.end(),
          "and without the original seeing the new key");
}

} // namespace

int main()
{
    an_empty_map_ends_where_it_begins();
    an_empty_map_iterates_over_nothing();
    a_map_emptied_again_still_ends_where_it_begins();

    the_map_is_sorted_by_key();
    every_key_is_found_and_no_others();
    a_repeated_key_replaces_its_value();
    erasing_removes_exactly_one();
    erasing_a_range_removes_exactly_it();
    erasing_an_empty_range_does_nothing();
    emplace_puts_the_key_where_insert_would();
    the_map_holds_its_capacity();
    a_moved_from_map_is_empty();
    a_copy_is_independent();

    std::println(
        "small_map: {} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
