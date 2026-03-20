# Index Rebuild

Rebuild allows you to reconstruct an HNSW index graph with new configuration parameters (M, ef_construction) without re-uploading vector data. All vectors are re-indexed from MDBX storage — only the graph structure is rebuilt.

## API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/v1/index/{name}/rebuild` | Start async rebuild |
| GET | `/api/v1/index/{name}/rebuild/status` | Check rebuild progress |

---

## Start Rebuild

**POST** `/api/v1/index/{name}/rebuild`

All parameters are optional. Omitted parameters retain their current values.

```json
{
    "M": 32,
    "ef_con": 256
}
```

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `M` | int | HNSW graph connectivity (4–512) |
| `ef_con` | int | Construction-time search quality (8–4096) |

**Response 202:**
```json
{
    "status": "rebuilding",
    "previous_config": { "M": 16, "ef_con": 128 },
    "new_config": { "M": 32, "ef_con": 256 },
    "total_vectors": 50000
}
```

**Errors:**

| Code | Condition |
|------|-----------|
| 400 | No changes specified, invalid parameters, or attempted to change `precision`/`space_type` |
| 404 | Index not found |
| 409 | Rebuild or backup already in progress for this user |

---

## Check Progress

**GET** `/api/v1/index/{name}/rebuild/status`

```json
{
    "status": "in_progress",
    "vectors_processed": 45000,
    "total_vectors": 100000,
    "percent_complete": 45.0
}
```

When no rebuild is active, `status` is `"idle"`.

---

## Restrictions

The following parameters **cannot** be changed via rebuild (returns 400):
- `precision` (quantization level)
- `space_type`


---

## Behavior

- **All vectors are re-indexed** from MDBX storage into a new HNSW graph with the updated configuration.
- **Search continues** during rebuild — queries use the old index until the rebuild completes.
- **Write operations** (insert, delete, update) will block and timeout while the rebuild is running, same as during backup.
- **One rebuild at a time per user** — cannot start a rebuild on any index while another rebuild is in progress for the same user. Also cannot run concurrently with a backup.
- **Periodic checkpoints** — the in-progress graph is saved to a temp file at regular intervals.
- **On completion**, the new graph is saved as a timestamped file (e.g., `default.idx.1710745200`) which is kept permanently, then copied to `default.idx`.
- **On server restart** during an incomplete rebuild, the old index loads normally. Temp files are cleaned up automatically. The rebuild must be restarted manually.
