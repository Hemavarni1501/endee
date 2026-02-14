# Backup System

Backups are stored as `.tar` archives in `{DATA_DIR}/backups/`. Job state is persisted to `jobs.json`.

## API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/v1/index/{name}/backup` | Create async backup |
| GET | `/api/v1/backups` | List all backup files |
| GET | `/api/v1/backups/jobs` | List all backup jobs with status |
| POST | `/api/v1/backups/{name}/restore` | Restore backup to new index |
| DELETE | `/api/v1/backups/{name}` | Delete a backup file |
| GET | `/api/v1/backups/{name}/download` | Download backup (streaming) |
| POST | `/api/v1/backups/upload` | Upload a backup file |

---

## Two Mutexes

```
jobs_mutex_ (shared_mutex, global)       operation_mutex (mutex, per-index)
├── Protects: backup_jobs_,              ├── Protects: index data
│   active_backup_jobs_ maps             ├── Scope: single index
├── Scope: all indexes                   └── Held for: seconds/minutes
├── Held for: microseconds                   (save + tar creation)
└── Shared reads, exclusive writes
```

**Why not just operation_mutex?** Backup thread holds `operation_mutex` for minutes (save + tar). If writes checked that same mutex, the HTTP request would hang. With separate `jobs_mutex_`, writes check the fast map, see backup is active, and immediately return 409.

**No circular dependency** — neither thread holds both mutexes at the same time:

```
Write:   lock jobs_mutex_ → release → lock operation_mutex → release
Backup:  lock jobs_mutex_ → release → lock operation_mutex → release → lock jobs_mutex_ → release
```

---

## Flows

### Create Backup (Async)

```
POST /index/X/backup → validateBackupName() → check no duplicate on disk
→ generate job_id → [LOCK jobs_mutex_] register job + persist [UNLOCK]
→ spawn detached thread → return 202 { job_id }
```

**Background thread** (`executeBackupJob`):

```
[LOCK jobs_mutex_] verify job exists [UNLOCK]
→ check disk space (need 2x index size) → read metadata
→ [LOCK operation_mutex] saveIndexInternal → write metadata.json → create .tmp_{name}.tar → cleanup metadata.json [UNLOCK operation_mutex]
→ [LOCK jobs_mutex_] erase from active_backup_jobs_ [UNLOCK]
→ rename .tmp_ → final tar (atomic)
→ [LOCK jobs_mutex_] mark COMPLETED + persist [UNLOCK]
```

**On failure**: cleanup temp files → mark job FAILED → erase from active_backup_jobs_ → persist.

### Write During Backup

```
addVectors/deleteVectors/updateFilters/deleteByFilter/replaceVector
→ checkBackupInProgress(): [SHARED LOCK jobs_mutex_] check active_backup_jobs_ [UNLOCK]
→ if backup active: immediately throw 409 "Cannot modify index while backup is in progress"
→ if no backup: [LOCK operation_mutex] do the write [UNLOCK] → 200 OK
```

### Restore Backup

```
POST /backups/{name}/restore
→ validate name → check tar exists → check target index does NOT exist
→ extract tar → read metadata.json → copy files to target dir
→ register in MetadataManager → cleanup temp dir → loadIndex()
→ 201 OK
```

### Download (Streaming)

```
GET /backups/{name}/download
→ check file exists → set_static_file_info_unsafe() (Crow streams from disk in chunks)
→ Server RAM stays constant (~8 MB) even for 23 GB+ files
```

### Upload

```
POST /backups/upload (multipart)
→ parse multipart → validate .tar extension + name → check no duplicate → write to disk
→ 201 OK

NOTE: Upload currently buffers entire file in RAM (Crow multipart parser limitation).
```

---

## Safety Checks

| # | Check | Where |
|---|-------|-------|
| 1 | **One backup per index** — active_backup_jobs_ allows only one running backup per index | createBackupAsync, checkBackupInProgress |
| 2 | **Write protection** — all write ops check backup status first, get instant 409 if active | addVectors, deleteVectors, updateFilters, deleteByFilter, replaceVector |
| 3 | **Name validation** — alphanumeric, underscores, hyphens only; max 200 chars | validateBackupName |
| 4 | **Duplicate prevention** — checked at creation AND inside background thread | createBackupAsync, executeBackupJob, upload |
| 5 | **Disk space** — requires 2x index size available | executeBackupJob |
| 6 | **Atomic tar** — writes to .tmp_ first, then renames | executeBackupJob |
| 7 | **Crash recovery** — on startup: mark stale IN_PROGRESS as FAILED, delete .tmp_ files | loadJobs, cleanupIncompleteBackups |
| 8 | **Restore safety** — target must not exist, metadata must be valid, cleanup on failure | restoreBackup |
| 9 | **Job persistence** — atomic write (tmp + rename) to jobs.json on every status change | persistJobs |
