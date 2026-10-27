#include "command_list_notes.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
using phantom_fx::CommandListNotes;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Notes as a module keeps them: values that append in the order they run, and a flag that adds up.
struct TestNotes
{
    std::vector<int> values;
    bool flag{};

    void merge(TestNotes later)
    {
        values.insert(values.end(), later.values.begin(), later.values.end());
        flag = flag || later.flag;
    }
};

using Notes = CommandListNotes<TestNotes>;

// Deferred contexts and command lists, by address.
constexpr std::uint64_t Context = 0x1000, OtherContext = 0x2000, List = 0x3000, OtherList = 0x4000;

void add(Notes& notes, std::uint64_t context, int value)
{
    notes.note(context, [&](TestNotes& noted) { noted.values.push_back(value); });
}

std::vector<int> valuesOf(const std::optional<TestNotes>& taken)
{
    return taken ? taken->values : std::vector<int>{};
}

void finishMovesNotesToList()
{
    Notes notes;
    add(notes, Context, 1);
    add(notes, Context, 2);
    notes.note(Context, [](TestNotes& noted) { noted.flag = true; });
    notes.recorded(List, Context);

    const auto taken = notes.take(List);
    expect(taken && taken->values == std::vector{1, 2} && taken->flag, "A list hands over what its context noted");
}

void contextStartsEmptyAfterFinish()
{
    Notes notes;
    add(notes, Context, 1);
    notes.recorded(List, Context);
    add(notes, Context, 2);
    notes.recorded(OtherList, Context);

    expect(valuesOf(notes.take(List)) == std::vector{1}, "A list holds what its context noted before the finish");
    expect(valuesOf(notes.take(OtherList)) == std::vector{2}, "The context's next list holds only what it noted after");
}

void handedOverOnce()
{
    Notes notes;
    add(notes, Context, 1);
    notes.recorded(List, Context);

    expect(notes.take(List).has_value(), "A finished list's notes are handed over");
    expect(!notes.take(List), "A list's notes are handed over once");
}

void nestedListJoinsInExecutionOrder()
{
    Notes notes;
    add(notes, OtherContext, 2);
    notes.recorded(OtherList, OtherContext);

    // The context notes 1, runs the other list inside it, notes 3 and finishes.
    add(notes, Context, 1);
    notes.recorded(Context, OtherList);
    add(notes, Context, 3);
    notes.recorded(List, Context);

    expect(valuesOf(notes.take(List)) == std::vector{1, 2, 3}, "A nested list's notes come between the context's, as they run");
    expect(!notes.take(OtherList), "A nested list's notes are handed over with its context's, not again");
}

void nestedListIntoQuietContext()
{
    Notes notes;
    add(notes, OtherContext, 2);
    notes.recorded(OtherList, OtherContext);
    notes.recorded(Context, OtherList);
    add(notes, Context, 3);
    notes.recorded(List, Context);

    expect(valuesOf(notes.take(List)) == std::vector{2, 3}, "A context that noted nothing yet takes a nested list's notes");
}

void forgottenListGivesNothing()
{
    Notes notes;
    add(notes, Context, 1);
    notes.recorded(List, Context);
    notes.forget(List);

    expect(!notes.take(List), "A list gone before it ran gives nothing");
}

void reusedContextAddressStartsEmpty()
{
    Notes notes;
    add(notes, Context, 1);
    notes.forget(Context);
    add(notes, Context, 2);
    notes.recorded(List, Context);

    expect(valuesOf(notes.take(List)) == std::vector{2}, "A context made at a gone one's address starts empty");
}

void expiresAfterThreePresents()
{
    Notes notes;
    add(notes, Context, 1); // Never finished
    add(notes, OtherContext, 2);
    notes.recorded(List, OtherContext); // Never run
    for (int present = 1; present <= 3; ++present) expect(notes.present() == 0, "Notes wait through three presents");

    expect(notes.present() == 2, "At the fourth present a context's and a list's notes expire");
    expect(!notes.take(List), "An expired list gives nothing");

    notes.recorded(OtherList, Context);
    expect(!notes.take(OtherList), "An expired context's notes are gone");
}

void nextFrameRecordedDuringPresent()
{
    Notes notes;
    add(notes, Context, 1);
    notes.recorded(List, Context);
    // The next frame is recorded on the same context while this one runs and presents.
    add(notes, Context, 2);
    expect(valuesOf(notes.take(List)) == std::vector{1}, "The list run holds only its own frame's notes");
    expect(notes.present() == 0, "A frame being recorded keeps its notes through a present");

    notes.recorded(OtherList, Context);
    expect(valuesOf(notes.take(OtherList)) == std::vector{2}, "The next frame's list holds its notes");
}

void waitingSeesFinishedListsOnly()
{
    Notes notes;
    add(notes, Context, 1);
    notes.recorded(List, Context);
    add(notes, OtherContext, 2);
    notes.recorded(OtherList, OtherContext);
    notes.take(OtherList);
    add(notes, Context, 3);

    std::vector<int> seen;
    notes.withWaiting([&](auto&& waiting) {
        for (auto& noted : waiting)
        {
            seen.insert(seen.end(), noted.values.begin(), noted.values.end());
            noted.flag = true;
        }
    });

    expect(seen == std::vector{1}, "Only finished lists not run yet are waiting");

    const auto taken = notes.take(List);
    expect(taken && taken->flag, "A waiting list hands over what was changed in it");
}

void unknownKeysDoNothing()
{
    Notes notes;
    notes.recorded(List, Context);
    notes.forget(OtherList);

    expect(!notes.take(List), "A list from a context that noted nothing gives nothing");
    expect(notes.present() == 0, "Nothing expires where nothing was noted");
}
}

int main()
{
    try
    {
        finishMovesNotesToList();
        contextStartsEmptyAfterFinish();
        handedOverOnce();
        nestedListJoinsInExecutionOrder();
        nestedListIntoQuietContext();
        forgottenListGivesNothing();
        reusedContextAddressStartsEmpty();
        expiresAfterThreePresents();
        nextFrameRecordedDuringPresent();
        waitingSeesFinishedListsOnly();
        unknownKeysDoNothing();
        std::cout << "Command list notes tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
