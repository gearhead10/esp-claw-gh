---
{
  "name": "cap_webhook",
  "description": "Trigger user-configured outbound webhooks (Slack, Discord, IFTTT, n8n, Home Assistant, custom HTTP endpoints).",
  "metadata": {
    "cap_groups": [
      "cap_webhook"
    ],
    "manage_mode": "readonly"
  }
}
---

# Webhook

Use this skill when the user wants the device to notify, ping, or push data to an external service through a pre-configured HTTP endpoint.

## When to use
- The user asks to "notify", "ping", "send to Slack/Discord/Telegram via webhook", "trigger IFTTT", "fire my n8n flow", "tell Home Assistant", etc.
- The user wants to forward an event, alarm, or measurement to an external system.
- The user wants to test that a configured webhook works.

## When NOT to use
- The user wants to **read** information from the web → use `cap_web_search` instead.
- The user wants to talk to a chat user (Telegram/QQ DM, Web IM) → use the IM capability instead.
- The user wants to invoke an arbitrary URL the agent invented → **refuse**. Webhooks must be in the registry.

## Available capabilities
- `webhook_list`: returns the registry of webhooks the user has pre-configured in the device UI.
- `webhook_trigger`: fires one of the registered webhooks by name, with an optional JSON payload.

## Calling rules

**Always call `webhook_list` first** before `webhook_trigger`, unless the user explicitly named a webhook and you already know it exists from a recent `webhook_list` call in this turn.

You **cannot** make up webhook URLs. If a webhook the user requests is not in the list, tell them and stop. Do not retry with a guessed name.

### `webhook_list`
Input: empty object `{}`.
Output: JSON object with a `webhooks` array. Each item has `name`, `method`, and optionally `description`.

```json
{"webhooks":[
  {"name":"slack_ops","method":"POST","description":"Slack #ops channel"},
  {"name":"n8n_temp_alert","method":"POST","description":"n8n flow when temp > 30C"}
]}
```

### `webhook_trigger`
Input JSON:

```json
{
  "name": "slack_ops",
  "payload": {
    "text": "Device temperature reached 31.4C"
  }
}
```

- `name` (required): exact name from `webhook_list`.
- `payload` (optional): JSON object sent as the request body for non-GET methods. For Slack you typically need `{"text": "..."}`; for Discord `{"content": "..."}`; for IFTTT `{"value1": "...", "value2": "..."}`; for n8n whatever your flow expects.

Output JSON:

```json
{
  "name": "slack_ops",
  "method": "POST",
  "ok": true,
  "status": 200,
  "response_preview": "ok"
}
```

If the call failed at the transport level, `ok` is `false` and the result has an `error` string instead of `status`.

## Provider-agnostic payload tips
| Destination | Typical payload field |
|---|---|
| Slack incoming webhook | `text` |
| Discord webhook | `content` |
| IFTTT Maker | `value1`, `value2`, `value3` |
| n8n / Zapier / Make | whatever your flow expects |
| Home Assistant webhook | flat JSON, any fields |

If you're unsure of the payload shape, send a minimal `{"message": "..."}` and tell the user to check their service's expected format if delivery fails.

## Common failure causes
- The webhook registry is empty — tell the user to add a webhook in the device UI under the "webhook" section, supplying `name`, `url`, optional `method`, optional `description`.
- The user requested a webhook by a name that does not exist in the registry.
- The remote service rejected the payload (non-2xx `status`). Show the `response_preview` to the user so they can debug.
- Timeout (8 seconds) — the remote endpoint did not answer in time.

## Recommended workflow
1. Call `webhook_list`. Read the names available.
2. Decide which webhook fits the user's request.
3. Build a payload appropriate to the destination (see table above).
4. Call `webhook_trigger`.
5. Report `ok` + `status` (or `error`) back to the user. Include `response_preview` if it adds value.

## Examples

Plain notification, no payload:

```json
{
  "name": "n8n_doorbell"
}
```

Slack message:

```json
{
  "name": "slack_ops",
  "payload": { "text": "Edge agent started after reboot" }
}
```

Forward a temperature reading to a custom flow:

```json
{
  "name": "n8n_temp_alert",
  "payload": { "device": "esp-claw", "temp_c": 31.4, "rssi": -54 }
}
```
