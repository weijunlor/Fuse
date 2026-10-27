# Assignment Brief (summary)

Summary of the CSD3116 Team Lab Assignment, "Real-Time, Interactive, Distributed and Secure Simulation". The original PDF is kept outside this repo.

## Game requirements

1. Must be 2D/3D animation-based.
2. Must use WSL, OpenGL, GLFW and Cyclone DDS (macOS or native Linux allowed in place of WSL).
3. Must support at least two players.
4. Must be real-time, interactive, distributed and secure.
5. Should improve on the baseline programs, e.g. support a client leaving the game.
6. Must apply concepts from class:
   - Optimised code, e.g. better threading, avoiding repeated computation in loops.
   - Security: authentication of clients, encryption of DDS traffic.

## Baseline rules (lecturer's programs)

- Server is the only writer of the game state (ground truth), an array indexed by player ID − 1.
- Clients mirror the array from server `Position` messages and render only active players.
- Server and client each have a main thread and a DDS worker thread; shared variables are protected by a mutex.

## Deliverables — due Friday 6 November 2026, Moodle

1. **Source code (.zip)**, clearly commented.
2. **Slides (.pdf)** covering:
   - Build and run instructions
   - Which team member did what
   - Game concept, mechanics, design (software engineering diagrams)
   - Code optimisations done, validated with tools such as profilers
   - Security features implemented
   - If AI generated code: proof it was tested (code walkthrough, defence of accepted output)
3. **Video (.mp4, ≤ 200 MB, ≤ 5 min)**:
   - Narrated by all team members, no AI voices (AI captions allowed)
   - Video of members narrating is recommended
   - Identify the team and members, demo the game, describe optimisations and security
4. **AI declaration** in every deliverable: summary of prompts used and how generated code was tested.

## Grading

| Deliverable | Weight | Breakdown |
|---|---|---|
| Program | 40% | Runs 20%, look-and-feel 10%, **optimisation and security 40%**, structure/modularity 10%, readability 10%, comments 10% |
| Slides | 20% | Clarity 25%, structure 25%, technical content 50% |
| Video | 20% | Clarity/organisation, technical content, demo/visuals, communication — 25% each |
| Peer evaluation | 20% | Instructions given near the deadline |

Slide rubric top band: simple design, large font, not too much per slide; technical content shows critical analysis, not general background.
