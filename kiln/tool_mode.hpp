#pragma once

namespace CLI {
class App;
}

namespace kiln {

// Registers all `kiln tool <command>` sub-subcommands on `tool`. Each
// sub-subcommand has its own --help, options, and validation provided by
// CLI11. The selected command's exit status is written to `exit_code` from
// the callback; main inspects it after CLI11_PARSE returns.
void register_tool_subcommands(CLI::App* tool, int& exit_code);

} // namespace kiln
