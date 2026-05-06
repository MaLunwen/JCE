# Contributing to JCE

Thank you for your interest in contributing to the **JCE (Java Cat Engine)** project!

## Getting Started

1. Fork the repository and create your branch from `main`.
2. Follow the build instructions in [README.md](README.md) to set up your development environment.
3. Make your changes, ensuring existing tests and builds still pass.
4. Open a pull request with a clear description of what you changed and why.

## Code Style

- C source files follow C99 conventions.
- Editor/tool code uses C++17.
- Keep functions small and single-purpose.
- All new direct third-party dependencies require explicit project-owner approval and must be listed in `README.md` and `THIRD_PARTY_LICENSES.md`.

## Commit Messages

Use short, descriptive imperative-mood commit messages, e.g.:

```
fix: correct UV wrap mode for atlas textures
feat: add ARM64 build profile for macOS
docs: update Windows build prerequisites
```

## Reporting Issues

Please open a GitHub Issue with:

- A clear title and description.
- Steps to reproduce (if it's a bug).
- The platform, compiler version, and build variant you are using.

## License

By contributing, you agree that your contributions will be licensed under the same license as the project.
