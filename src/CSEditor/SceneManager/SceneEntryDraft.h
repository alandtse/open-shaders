#pragma once

#include <cassert>
#include <span>
#include <vector>

/** @brief Retains one scene editor's entries while its preview is suspended or saved. */
template <class Entry>
class SceneEntryDraft
{
public:
	/** @brief Starts with the matching entries; unrelated entries remain owned by their caller. */
	template <class Matches>
	void Begin(const std::vector<Entry>& entries, Matches matches)
	{
		saved = Select(entries, matches, &savedPositions);
		working = saved;
		workingPositions = savedPositions;
		active = true;
	}

	/** @brief Whether the preview entries differ from the last successful save. */
	template <class Matches>
	bool HasChanges(const std::vector<Entry>& entries, Matches matches) const
	{
		if (!active)
			return saved != working;
		auto original = saved.begin();
		for (const auto& entry : entries) {
			if (!matches(entry))
				continue;
			if (original == saved.end() || *original != entry)
				return true;
			++original;
		}
		return original != saved.end();
	}

	/** @brief Keeps the draft and restores only its persisted entries. */
	template <class Matches>
	void Suspend(std::vector<Entry>& entries, Matches matches)
	{
		if (!active)
			return;
		working = Select(entries, matches, &workingPositions);
		Replace(entries, saved, savedPositions, matches);
		active = false;
	}

	/** @brief Restores the retained preview without changing unrelated entries. */
	template <class Matches>
	void Resume(std::vector<Entry>& entries, Matches matches)
	{
		if (active)
			return;
		Replace(entries, working, workingPositions, matches);
		active = true;
	}

	/** @brief Promotes the current draft after its atomic save succeeded. */
	template <class Matches>
	void Accept(const std::vector<Entry>& entries, Matches matches)
	{
		saved = active ? Select(entries, matches, &savedPositions) : working;
		if (!active)
			savedPositions = workingPositions;
		working = saved;
		workingPositions = savedPositions;
	}

	/** @brief Whether the caller's entries currently include the preview. */
	bool IsActive() const { return active; }

	/** @brief Builds the persisted view while unrelated entries retain their current values. */
	template <class Matches>
	std::vector<Entry> SavedView(const std::vector<Entry>& entries, Matches matches) const
	{
		auto persisted = entries;
		Replace(persisted, saved, savedPositions, matches);
		return persisted;
	}

	/** @brief Captures authored contents independently of whether the preview is visible. */
	template <class Matches>
	std::vector<Entry> WorkingView(const std::vector<Entry>& entries, Matches matches) const
	{
		auto authored = entries;
		if (!active)
			Replace(authored, working, workingPositions, matches);
		return authored;
	}

	/** @brief Restores logical undo contents while a suspended preview keeps its saved live entries. */
	template <class Matches>
	void RestoreWorking(std::vector<Entry>& entries, Matches matches)
	{
		std::vector<size_t> positions;
		auto restored = Select(entries, matches, &positions);
		if (!active)
			Replace(entries, saved, savedPositions, matches);
		working.swap(restored);
		workingPositions.swap(positions);
	}

private:
	template <class Matches>
	static std::vector<Entry> Select(const std::vector<Entry>& entries, Matches matches,
		std::vector<size_t>* positions = nullptr)
	{
		std::vector<Entry> selected;
		if (positions)
			positions->clear();
		for (size_t index = 0; index < entries.size(); ++index)
			if (matches(entries[index])) {
				selected.push_back(entries[index]);
				if (positions)
					positions->push_back(index);
			}
		return selected;
	}

	template <class Matches>
	static void Replace(std::vector<Entry>& entries, const std::vector<Entry>& replacement,
		std::span<const size_t> positions, Matches matches)
	{
		assert(positions.size() == replacement.size());
		std::vector<Entry> next;
		next.reserve(entries.size() + replacement.size());
		auto unrelated = entries.begin();
		const auto advance = [&] {
			while (unrelated != entries.end() && matches(*unrelated))
				++unrelated;
		};
		advance();
		for (size_t index = 0; index < replacement.size() || unrelated != entries.end();) {
			if (index < replacement.size() && (unrelated == entries.end() || next.size() >= positions[index])) {
				next.push_back(replacement[index++]);
			} else {
				next.push_back(*unrelated++);
				advance();
			}
		}
		entries.swap(next);
	}

	std::vector<Entry> saved;
	std::vector<Entry> working;
	std::vector<size_t> savedPositions;
	std::vector<size_t> workingPositions;
	bool active = false;
};
