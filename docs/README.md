# docs/

Background reading for `ts_partition`. Use the source as the ground
truth; these documents exist to make the design legible at a glance.

| document                                  | what it covers                                                                                                  |
| ----------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| [architecture.md](./architecture.md)      | Component layout, class diagram of the in-memory types, query lifecycle, cursor state machine, file layout.     |
| [sql-routing.md](./sql-routing.md)        | How one parent `SELECT` becomes N child `SELECT`s. xBestIndex constraint encoding, xFilter date enumeration, cursor advance loop, push-down construction, LRU interactions. |
| [design-decisions.md](./design-decisions.md) | Why each non-obvious choice was made. Read-only by design; schema sniffed at xConnect; file list re-derived per query; LRU only for historical; no ATTACH; omit=0; the 30s progress budget; etc. |

All diagrams are Mermaid. GitHub renders them inline. To preview
locally, paste into <https://mermaid.live> or install the Mermaid CLI.
