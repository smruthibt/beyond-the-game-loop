#pragma once
#include "Transport.hpp"
#include <SDL3/SDL.h>

namespace towerfall {
struct ScreenState {
  bool telemetry = false, help = false, demo = false;
  bool options = false, paused = false;
  bool captureRequested = false;
  // Quit confirmation: drops down over play or Options; choice 1 is "yes".
  bool quitPrompt = false;
  int quitChoice = 0;
  double quitOpened = 0;
  int option = 0;
  double speed = 1;
  double realSeconds = 0, personalSeconds = 0;
  uint64_t loops = 0;
  int aim = 0;
  // Any overlay that takes the keyboard away from gameplay.
  bool MenuOpen() const { return options || help || quitPrompt; }
};
class Painter {
public:
  explicit Painter(SDL_Renderer *);
  void Game(const NetworkFrame &, const ScreenState &, Mode);
  bool Screenshot(const std::string &);

private:
  void Fill(float, float, float, float, SDL_Color);
  void Stroke(float, float, float, float, SDL_Color);
  void Label(float, float, const std::string &, float, SDL_Color);
  void Circle(float, float, float, SDL_Color);
  void Triangle(float, float, float, float, float, float, SDL_Color);
  void Backdrop(double, float moonX = 960, float moonY = 335);
  void Stone(Rect, int, bool = false);
  void ArcherSprite(const Archer &, int, bool);
  void ArrowSprite(const Arrow &);
  void Torch(float, float, double);
  SDL_Renderer *renderer;
};
class Audio {
public:
  void Open();
  void Note(float frequency, float length, float volume = .06f);
  void Close();
  ~Audio() { Close(); }

private:
  SDL_AudioStream *stream = nullptr;
};
} // namespace towerfall
