# SPP I/O Capability Matrix

This matrix reflects `spp::IO::capability_matrix()` in `include/spp/io/handle.h`.

| Capability | Linux | macOS | Windows |
| --- | --- | --- | --- |
| `file_handles` | ✅ | ✅ | ✅ |
| `socket_handles` | ✅ | ✅ | ✅ |
| `pipe_handles` | ✅ | ✅ | ✅ |
| `poll_file` | ✅ (`poll`) | ✅ (`poll`) | ❌ |
| `poll_socket` | ✅ (`poll`) | ✅ (`poll`) | ✅ (`WSAPoll`) |
| `poll_pipe` | ✅ (`poll`) | ✅ (`poll`) | ❌ |
| `edge_triggered` | ❌ | ❌ | ❌ |
| `io_uring` | ✅ | ❌ | ❌ |
| `kqueue` | ❌ | ✅ | ❌ |
| `iocp` | ❌ | ❌ | ✅ |

## Unified API Surface

- `IO::Handle` unifies File/Socket/Pipe handles.
- `IO::poll_any_result` and `IO::poll_one_result` provide a pollable event model.
- `IO::pipe_result`, `IO::read_some_result`, `IO::write_some_result`, `IO::close_result` provide baseline cross-platform handle operations.
