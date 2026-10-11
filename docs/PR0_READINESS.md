# PR0 — gta3-vr

**EXPERIMENTAL.** Kept private while the items below are resolved. See the [README](../README.md) for the port's features, controls, game-data requirements and build instructions.

| Record | Revision |
| --- | --- |
| Hygiene audit | `8b19bf2efb25073e5e8ba9357c25138e46c68de4` |
| Key provenance review source | `3cc0388ec8265df9ced494511d5d43bd70aa679b` |
| Build/test source | No fresh full-build SHA recorded in this audit. |

Target: Steam Frame / Lepton Android 11 API 30 / ARM64 / SteamVR OpenXR / Vulkan multiview.

No fresh build or tests were run for the audited source. The untracked scratchpad/ directory was preserved and excluded from the audit. No new full application build is claimed for this audit.

The publication blockers are:

- Reachable history contains 30 commercial game TXD textures. Removing them at HEAD would leave the historical objects available.
- Each of two historical `premake5Linux` binaries contains a key block that exactly matches the published [Mbed TLS 1.3.22 test fixture](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-1.3.22/tests/data_files/server2.key). That matching block is test material. Other embedded private-key blocks remain unclassified and need provenance review.

Retail game files are external inputs. Existing host checks do not establish device behavior. Keep the original upstream and component notices; they do not grant rights to the commercial game.

[Exact metadata](PR0_METADATA.json) · [Changelog](../CHANGELOG.md)
