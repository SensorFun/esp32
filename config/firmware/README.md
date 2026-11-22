# Flashable firmware

Each `<project>.yaml` here makes `projects/<project>` available in the browser
flasher and in CI. A project without a file is not built or offered.

| Field | Meaning |
|---|---|
| `name`, `description` | Shown in the flasher. |
| `boards` | `all`, or a list of board IDs from `config/boards`. |
| `requires_frame_grabber` | Optional; `true` limits `boards` to boards listing a frame grabber. |
| `private_settings` | Optional sdkconfig names; a build with any of them set is never published. |
| `after_flash` | Optional `text` and `link` shown after flashing. Not shown when the project has `provisioning`. |
| `provisioning` | Optional. `page` is a workspace path to the setup page the flasher's **Provision** popup shows, for example `projects/common/provisioning/index.html`. |

`python3 tools/firmware_artifacts.py --list` prints the resulting project and
board pairs; CI builds exactly those.

## Provisioning

When a project lists `provisioning.page`, the flasher shows **Provision** next
to the flash button and opens it after flashing that project. The popup runs
the page unchanged and answers its `/api/scan`, `/api/connect`, `/api/status`
and `/api/finish` requests over USB, using the commands of
`projects/common/provisioning`. A project gets this by using that component,
and its page may change freely as long as it keeps those requests.

Before showing the page, the popup asks the board which project it runs (the
firmware's CMake `project()` name, which must match the `<project>.yaml` file
name). If the board runs another project, or firmware too old to say, the
popup explains what to select or flash instead and does not provision it.
