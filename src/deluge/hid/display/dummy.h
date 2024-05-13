#pragma once

#include "hid/display/display.h"
namespace deluge::hid::display {
class Dummy : public Display {
public:
	Dummy() : Display(DisplayType::DUMMY) {}
	~Dummy() override = default;

	constexpr size_t getNumBrowserAndMenuLines() override { return 1; };

	void displayPopup(char const* newText, int8_t numFlashes = 3, bool alignRight = false, uint8_t drawDot = 255,
	                  int32_t blinkSpeed = 1, DisplayPopupType type = DisplayPopupType::GENERAL) override{};

	void popupText(char const* text, DisplayPopupType type = DisplayPopupType::GENERAL) override{};
	void popupTextTemporary(char const* text, DisplayPopupType type = DisplayPopupType::GENERAL) override{};

	void cancelPopup() override{};
	void freezeWithError(char const* text) override{};
	bool isLayerCurrentlyOnTop(NumericLayer* layer) override { return false; };
	void displayError(Error error) override{};

	void removeWorkingAnimation() override{};

	// Loading animations
	void displayLoadingAnimationText(char const* text, bool delayed = false, bool transparent = false) override{};
	void removeLoadingAnimation() override{};

	bool hasPopup() override { return false; };
	bool hasPopupOfType(DisplayPopupType type) override { return false; };

	void consoleText(char const* text) override{};

	void timerRoutine() override{};
};
} // namespace deluge::hid::display
