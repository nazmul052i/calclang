# CalcLang for VSCode

A minimal extension that gives `.clc` and `.casm` files proper editor support:

- Syntax highlighting (keywords, types, calclib builtins, strings, numbers, comments, operators)
- **File icons** in the explorer and tabs — a purple **CL** badge for `.clc` files and an orange **AS** badge for `.casm` / `.co` / `.cexe` files
- Block-comment toggle (`/* ... */`) and line-comment toggle (`//`)
- Auto-closing `{}`, `[]`, `()`, and `""`
- Bracket matching and surrounding pairs

No language server (yet) — no IntelliSense, no go-to-definition. Just colors, icons, and the comfortable editor mechanics you'd want when writing more than a few lines.

## File icon preview

| Extension       | Icon            | Color hex     |
|-----------------|-----------------|---------------|
| `.clc`         | rounded **CL**  | `#5E35B1` (indigo) |
| `.casm` / `.co` / `.cexe` | rounded **AS**  | `#E64A19` (deep orange) |

Both icons are SVGs in `icons/`; you can swap the `<rect>` `fill` or `<text>` content if you want to rebrand.

## Requirements

VSCode 1.61.0 or later (file-icon support on language contributions was added in 1.61).

## Install (Windows)

1. Close VSCode if it's open.
2. Copy this folder (`editor/vscode/calclang/`) into your VSCode extensions directory:

   ```
   %USERPROFILE%\.vscode\extensions\calclang\
   ```

3. Reopen VSCode and open a `.clc` file. It should now show syntax highlighting in the status bar's language indicator.

## Install (macOS / Linux)

```bash
cp -r editor/vscode/calclang ~/.vscode/extensions/
```

Then reopen VSCode.

## Development install (live-edit the grammar)

Symlink instead of copy, then iterate on the grammar without re-copying:

**Windows (PowerShell, run as admin or with Developer Mode on):**
```powershell
New-Item -ItemType SymbolicLink `
    -Path "$env:USERPROFILE\.vscode\extensions\calclang" `
    -Target "$PWD\editor\vscode\calclang"
```

**macOS / Linux:**
```bash
ln -s "$PWD/editor/vscode/calclang" ~/.vscode/extensions/calclang
```

After changing any of the JSON files, run `Developer: Reload Window` from the Command Palette (Ctrl+Shift+P).

## Optional: a one-keystroke "compile and run" task

Drop this into `.vscode/tasks.json` in your CalcLang project. Press `Ctrl+Shift+B` (default Build keybinding) on an open `.clc` file and it'll compile and run it through the VM via the `clc` driver:

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Run current .clc",
            "type": "shell",
            "command": "${workspaceFolder}/bin/clc.exe",
            "args": ["-r", "${file}"],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "presentation": {
                "reveal": "always",
                "panel": "shared"
            },
            "problemMatcher": []
        }
    ]
}
```

On macOS / Linux drop the `.exe` (`${workspaceFolder}/bin/clc`). Swap `-r` for `-o ${fileDirname}/${fileBasenameNoExtension}.exe` if you'd rather build a native executable than run on the VM.

## What it doesn't do (yet)

- **IntelliSense / autocomplete**: would require a language server (LSP). Significant scope expansion.
- **Inline error diagnostics**: same — needs LSP. For now, build errors appear in the integrated terminal when you run the task.
- **Go-to-definition**: ditto.
- **Hover docs**: ditto.

If you want any of these, the way forward is to write a small LSP wrapper that runs `calcc` in `--ast` mode and translates parse errors into diagnostics, plus uses the AST output to build a symbol index. That's a separate project worth ~100 lines of TypeScript.

## Known visual quirks

- `num`, `str`, `arr`, `map`, `bool`, `any` are highlighted as types even when used as variable names (e.g. `let num = 5;` colors `num` as a type). The grammar can't tell type position from expression position without lookbehind on `:`. Cosmetic only; CalcLang itself happily accepts these as ordinary identifiers outside type position.
- `fn` is always highlighted as a keyword, including when used as a type annotation (`x: fn`). Same cosmetic limitation.
