# SANAD MCP server

The local stdio MCP server gives a compatible AI host tools for one selected SANAD project. Python 3.9+ and a built `nf.exe` are required. Build the engine with `build_nf.bat Release` first.

Example MCP host configuration (replace paths with actual absolute paths):

```json
{
  "mcpServers": {
    "sanad": {
      "command": "python",
      "args": ["C:\\SANAD\\Tools\\MCP\\server.py", "--project", "C:\\Games\\MyGame\\MyGame.nfproj", "--nf", "C:\\SANAD\\build\\Release\\bin\\nf.exe"]
    }
  }
}
```

Connect a model through an MCP-compatible host; the server itself does not supply a model or API key. `project_info`, `list_files`, `read_file`, `write_file`, and `run_project` operate on the chosen project. `write_file` requires the SHA-256 returned by `read_file`, accepts existing text files only, and writes to disk. `run_project` builds and runs headlessly with validation (up to 600 frames). Project build/run can create or overwrite generated Cache and dist output. The server cannot see unsaved editor changes, control the live editor, or provide editor undo. Close or reload an edited scene before using disk changes in the editor; do not use file editing alongside unsaved work. Only connect trusted MCP hosts: they can read and edit project files and execute its game scripts through run_project.

Run the integration test from the repository root: `python Tools/MCP/test_server.py`. It uses `.nftest` for its temporary project.


## Live editor tools (experimental)

Add `"--launch-editor"` to the configuration's `args` to launch a new editor instance for the selected project. The server then provides `editor_status`, `editor_list_entities`, `editor_create_primitive` and `editor_undo`. Primitive creation goes through the normal editor command and can be undone. Direct file writes are disabled while that instance runs, to avoid overwriting unsaved editor work. The bridge is opt-in and uses a temporary local directory; connect only trusted local clients and users. Closing the MCP host disconnects the bridge and does not close the editor. Existing editor instances cannot be attached. Changes are unsaved until saved in the editor. A timeout has an unknown outcome; inspect the editor before retrying.
