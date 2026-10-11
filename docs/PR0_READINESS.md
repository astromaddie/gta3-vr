# PR0 — GTA III on Steam Frame

**EXPERIMENTAL.** Personal source fork of re3 with the Steam Frame VR layer. See the [README](../README.md) for features, required retail data and build instructions.

Target: Steam Frame / Lepton Android 11 / ARM64 / SteamVR OpenXR / Vulkan multiview.

| Record | Revision |
| --- | --- |
| Earlier history audit | `8b19bf2efb25073e5e8ba9357c25138e46c68de4` |
| Host-tested source | `61b53c95996ef031277958034149a6f0d8c74254` |

The production camera scale and modifier tests, three controller binding profiles and Steam game-data resolver test passed without retail data. No fresh full APK build was run for this publication update.

The fork retains the upstream `gamefiles/models` TXD resources, `gamefiles/neo/neo.txd` and their historical versions. These came from upstream commits; the source license does not grant rights to Rockstar game media. The full retail game installation is supplied separately.

Each of the two historical `premake5Linux` binaries contains server and CA key blocks matching [official Mbed TLS test fixtures](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-1.3.22/library/certs.c). Other PEM fragments remain unclassified; the comparison did not demonstrate a project credential. The audit used hashes and exposed no key bytes.

Original upstream notices are retained. No APK or retail game archive is published. Existing local scratch work is excluded.

[Exact metadata](PR0_METADATA.json) · [Changelog](../CHANGELOG.md)
