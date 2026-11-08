#include "CSEditor/UndoHistory.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
	UndoHistory::Target MakeTarget(std::string key, std::optional<int>& value)
	{
		UndoHistory::Target target{ .key = key, .label = key };
		target.capture = [&value]() -> UndoHistory::SnapshotPtr {
			return value ? UndoHistory::Capture(*value) : nullptr;
		};
		target.restore = [&value](const UndoHistory::Snapshot& snapshot) {
			value = UndoHistory::Get<int>(snapshot);
		};
		return target;
	}
}

TEST_CASE("Editor history groups a drag until its interaction ends", "[editor][undo]")
{
	UndoHistory history;
	std::optional<int> value = 1;
	auto target = MakeTarget("widget:a", value);
	history.Watch(target, 1);
	value = 2;
	history.EndFrame(1, false);
	history.Watch(target, 2);
	value = 3;
	history.EndFrame(2, false);
	REQUIRE_FALSE(history.CanUndo());
	history.Watch(target, 3);
	history.EndFrame(3, true);
	REQUIRE(history.GetUndoCount() == 1);
	REQUIRE(history.Undo() == "widget:a");
	REQUIRE(value == 1);
	REQUIRE(history.Redo() == "widget:a");
	REQUIRE(value == 3);
}

TEST_CASE("Editor history skips no-ops and does not duplicate explicit changes", "[editor][undo]")
{
	UndoHistory history;
	std::optional<int> value = 1;
	auto target = MakeTarget("widget:a", value);
	history.Change(target, [] {});
	REQUIRE_FALSE(history.CanUndo());
	history.Watch(target, 1);
	history.Change(target, [&] { value = 2; });
	history.EndFrame(1, true);
	REQUIRE(history.GetUndoCount() == 1);
	REQUIRE(history.Undo());
	REQUIRE(value == 1);
	history.EndFrame(1, true);
	REQUIRE_FALSE(history.CanUndo());
	REQUIRE(history.GetRedoCount() == 1);
}

TEST_CASE("A new editor change invalidates redo", "[editor][undo]")
{
	UndoHistory history;
	std::optional<int> value = 1;
	auto target = MakeTarget("widget:a", value);
	history.Change(target, [&] { value = 2; });
	REQUIRE(history.Undo());
	REQUIRE(history.CanRedo());
	history.Change(target, [&] { value = 3; });
	REQUIRE_FALSE(history.CanRedo());
	REQUIRE(history.Undo());
	REQUIRE(value == 1);
}

TEST_CASE("Editor history drops unavailable targets and resolves older live targets", "[editor][undo]")
{
	UndoHistory history;
	std::optional<int> first = 1;
	std::optional<int> second = 2;
	auto a = MakeTarget("widget:a", first);
	auto b = MakeTarget("widget:b", second);
	history.Change(a, [&] { first = 10; });
	history.Change(b, [&] { second = 20; });
	second.reset();
	REQUIRE(history.Undo() == "widget:a");
	REQUIRE(first == 1);
	REQUIRE_FALSE(second);
	REQUIRE_FALSE(history.CanUndo());
}

TEST_CASE("Forgetting a source removes its history and watched baseline", "[editor][undo]")
{
	UndoHistory history;
	std::optional<int> value = 1;
	auto target = MakeTarget("widget:a", value);
	history.Watch(target, 1);
	history.Change(target, [&] { value = 2; });
	REQUIRE(history.Undo());
	history.Forget("widget:");
	REQUIRE_FALSE(history.CanUndo());
	REQUIRE_FALSE(history.CanRedo());
	value = 10;
	history.Watch(target, 2);
	history.EndFrame(2, true);
	REQUIRE_FALSE(history.CanUndo());
	value = 11;
	history.EndFrame(2, true);
	REQUIRE(history.Undo());
	REQUIRE(value == 10);
}

TEST_CASE("Returning editor targets establish a fresh baseline", "[editor][undo]")
{
	UndoHistory history;
	std::optional<int> value = 1;
	auto target = MakeTarget("widget:a", value);
	history.Watch(target, 1);
	history.EndFrame(1, false);
	value = 2;
	history.EndFrame(2, true);
	history.Watch(target, 3);
	history.EndFrame(3, true);
	REQUIRE_FALSE(history.CanUndo());
}

TEST_CASE("Editor history retains the latest one hundred changes", "[editor][undo]")
{
	UndoHistory history;
	std::optional<int> value = 0;
	auto target = MakeTarget("widget:a", value);
	for (int index = 1; index <= 101; ++index)
		history.Change(target, [&] { value = index; });
	REQUIRE(history.GetUndoCount() == 100);
	for (int index = 0; index < 100; ++index)
		REQUIRE(history.Undo());
	REQUIRE(value == 1);
	REQUIRE_FALSE(history.Undo());
}
