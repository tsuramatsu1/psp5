# Licences

psp5 as a whole is distributed under the **GPL-3.0-or-later**.

PPSSPP is GPL-2.0-**or-later**, so it may be combined with the GPL-3.0-or-later
platform layer and driver build; the result is GPL-3.0-or-later. (A GPL-2.0-*only*
program could not be, which is why the "or later" matters here.)

| Part | Licence | Source |
| --- | --- | --- |
| psp5's own code (`src/PS5*.cpp`, `src/PS5*.h`, `tooling/`, `tools/`) | GPL-3.0-or-later | this repository |
| `patches/ppsspp/ps5-port.patch` | GPL-2.0-or-later (it patches PPSSPP) | PS5_RetroArch |
| `src/platform/` | MIT | PS5_VulkanTemplate, © 2026 Mihawk |
| PPSSPP | GPL-2.0-or-later | fetched at build time, not vendored |
| RADV (Mesa) | MIT | linked from PS5_Vulkan's build |

The full texts belong with a release, not in the source tree: see
`tools/package.sh`.
