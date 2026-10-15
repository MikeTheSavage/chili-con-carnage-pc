# Profiles

Profiles contain everything PSPRecomp needs for a particular title without contaminating the reusable framework with game-specific addresses or behavior.

A profile is selected at configure time:

```bash
cmake -S . -B out/<profile> -DPSPRECOMP_PROFILE=<profile>
```

This repository includes `chili` (Chili Con Carnage, USA, ULUS-10216); upstream PSPRecomp also has `vcs`. See `docs/PROFILE_GUIDE.md` before adding another title.
