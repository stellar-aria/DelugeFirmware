#pragma once

// The audio-stream module's read seam. See docs/superpowers/specs/2026-07-15-audio-stream-module-design.md §6.
namespace deluge::audio::stream {

// Sanity anchor for Task 0; removed in Task 1 when the real interface lands.
bool module_linked();

} // namespace deluge::audio::stream
