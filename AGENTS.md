# Agent Instructions

- Do not assume `idf.py` is globally on PATH. On this Linux machine, use the generated ESP-IDF activation script in the same shell as the command, for example:

```bash
source "$HOME/.espressif/tools/activate_idf_v6.0.1.sh" >/tmp/idf-activate.log && idf.py build
```
