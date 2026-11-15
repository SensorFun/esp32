## Environment variables

Set and validate environment variables only in the launcher. Elsewhere, use plain `${VAR}` references; never use fallback or validation expressions such as `${VAR:-default}`, `${VAR-default}`, `${VAR:?message}`, or `${VAR?message}`, including in Docker Compose files.

Never put personal names, emails, credentials, or other user data in code.

## Websites

Always consider mobile usability when creating or changing websites. Use responsive layouts, readable text, touch targets at least 44px tall, and form inputs at least 16px on phones. Keep images and camera controls within the viewport without horizontal scrolling. Verify home pages and project interfaces at narrow phone, tablet, and desktop widths, including connected-camera settings when applicable.

## Docker

Never copy source code into Docker images. Mount source code read-only, and mount only the folders each container needs.

Never expose or publish container ports: do not use Dockerfile `EXPOSE`, Compose `ports`, or Docker `-p`/`--publish`. Use Docker networks for communication between containers.
