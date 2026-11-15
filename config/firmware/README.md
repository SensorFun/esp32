# Flashable firmware

Each `<project>.yaml` here makes `projects/<project>` available in the browser
flasher and in CI. A project without a file is not built or offered.

| Field | Meaning |
|---|---|
| `name`, `description` | Shown in the flasher. |
| `boards` | `all`, or a list of board IDs from `config/boards`. |
| `requires_frame_grabber` | Optional; `true` limits `boards` to boards listing a frame grabber. |
| `private_settings` | Optional sdkconfig names; a build with any of them set is never published. |
| `after_flash` | Optional `text` and `link` shown after flashing. |

`python3 tools/firmware_artifacts.py --list` prints the resulting project and
board pairs; CI builds exactly those.
