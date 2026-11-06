# Trophies on 13.42 (#13)

Findings, not done work. Status: on hold until the owner says which loader is running on the console. Nothing here calls a trophy service. `rt/` has no trophy code.

## RecompTrophy

[RecompTrophy](https://github.com/cptlamarca/RecompTrophy) makes PS5 trophy sets for Xbox 360 games. Its README asks for a jailbroken console that runs homebrew from `/data/homebrew/`, and for a payload loader on port 9021 (etaHEN, elfldr, or any other). It says the loader is how it writes where the console keeps trophies. The README names no firmware.

The v1.0.0 release is dated 4 Nov 2026. The release text read for this note does not name a firmware. [gta4-ps5 v1.1](https://github.com/elripalda/gta4-ps5/releases/tag/v1.1) (also 4 Nov 2026) ships native trophies and credits RecompTrophy. It asks for a payload loader (etaHEN, elfldr) and names no firmware.

No page read for this note says RecompTrophy works on 13.42. The team found no public report that it does. This note did not repeat a search of every forum. Treat "no public report" as the team's search, not as a new one.

## Loaders on 13.42

The transport has a path that people use on 13.42. Whether RecompTrophy's relay can write `/user/trophy2/` on 13.42 is unknown until the owner tries.

- One community report says the Relapse exploit's external elfldr is used on port 9021 on 13.42. **Unconfirmed.** The thread is [here](https://www.reddit.com/r/PS5_Jailbreak/comments/1wu13bw/ps5_1342_why_i_made_the_haze_autoloader_what/).
- Current [ps5-payload-dev elfldr](https://github.com/ps5-payload-dev/elfldr) is v0.27, published 21 Oct 2026.
- Official [etaHEN](https://github.com/etaHEN/etaHEN/releases/) 2.5B was published 25 Dec 2025. That is before 13.xx. The team says the only 13.42 report for etaHEN uses a locally repaired unofficial copy. **Unconfirmed** here. This note did not read that report.

[This post](https://x.com/elripalda/status/2117810028296208624) is listed with the gta4-ps5 work. This note did not re-read it.

## What this project would need

A loader on port 9021 that can write trophy files on 13.42, and then a try of RecompTrophy's relay against `/user/trophy2/`. Which loader the owner runs is the open question. Until that is known, trophies stay on hold.

## Sources

- https://github.com/cptlamarca/RecompTrophy
- https://github.com/elripalda/gta4-ps5/releases/tag/v1.1
- https://x.com/elripalda/status/2117810028296208624
- https://www.reddit.com/r/PS5_Jailbreak/comments/1wu13bw/ps5_1342_why_i_made_the_haze_autoloader_what/
- https://github.com/etaHEN/etaHEN/releases/
- https://github.com/ps5-payload-dev/elfldr
