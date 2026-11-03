# Plug history data

Written by `.github/workflows/history.yml` on `main`. Each
`history/<tid>/<YYYY-MM>.txt` line is `<seq> <base64>`: one batch from the
controller, sealed with that owner's key. Only the web app, signed in, can
read them. `state.json` tracks which batches arrived.
