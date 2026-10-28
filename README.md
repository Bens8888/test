# AutoLayout

A Geode mod for Geometry Dash 2.2 (2.2081, Windows and macOS) that designs complete, playable
layouts from a song, inside the editor. It works in two modes:

- **Automatic:** the click rhythm comes from the music.
- **Click recording:** you play along and the layout is built around your clicks.

Every layout is played by a built-in physics bot before you see it.

| | |
|---|---|
| Install | [docs/INSTALL.md](docs/INSTALL.md) |
| Use | [docs/USER_GUIDE.md](docs/USER_GUIDE.md) |
| Build (both platforms) | [docs/BUILD.md](docs/BUILD.md) |
| Retrain the models | [training/RETRAINING.md](training/RETRAINING.md) (Python audio mirror: [training/README.md](training/README.md)) |
| How it works | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/DECISIONS.md](docs/DECISIONS.md) |
| Known limitations | [docs/LIMITATIONS.md](docs/LIMITATIONS.md) |
| Roadmap | [docs/ROADMAP.md](docs/ROADMAP.md) |
| Preview gallery | [docs/GALLERY.md](docs/GALLERY.md) |
| Model evaluation | [training/models/EVALUATION.md](training/models/EVALUATION.md) |
| Benchmarks | [docs/BENCHMARKS.md](docs/BENCHMARKS.md) |

## Repository layout

```
core/       portable C++20 library: audio analysis, recordings, physics, bot, generator, model, data
src/        the Geode mod: menu, recorder hooks, editor bridge, session/jobs, ghost playback
tools/      alcli (analyze / generate / validate / bench / recordings), aldata (training data)
tests/      unit + integration tests for the core (run on every push)
training/   Python: audio analysis mirror (bit-exact), pattern model training, self-play loop
resources/  shipped pattern models (.alm)
assets/     demo click recording
docs/       documentation
```
