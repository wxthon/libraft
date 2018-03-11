#include "log.h"
#include "util.h"

// newLog returns log using the given storage. It recovers the log to the state
// that it just commits and applies the latest snapshot.
raftLog* newLog(Storage *storage, Logger *logger) {
  raftLog *log = new raftLog(storage, logger);

  uint64_t firstIndex, lastIndex;
  int err;

  err = storage->FirstIndex(&firstIndex);
  if (!SUCCESS(err)) {
    logger->Fatalf("get first index err:%s", GetErrorString(err));
  }

  err = storage->LastIndex(&lastIndex);
  if (!SUCCESS(err)) {
    logger->Fatalf("get last index err:%s", GetErrorString(err));
  }

  log->unstable_.offset_ = lastIndex + 1;
  log->unstable_.logger_ = logger;

  // Initialize our committed and applied pointers to the time of the last compaction.
  log->committed_ = firstIndex - 1;
  log->applied_    = firstIndex - 1;

  return log;
}

raftLog::raftLog(Storage *storage, Logger *logger) 
  : storage_(storage)
  , logger_(logger) {
}

// maybeAppend returns 0 if the entries cannot be appended. Otherwise,
// it returns last index of new entries.
uint64_t raftLog::maybeAppend(uint64_t index, uint64_t logTerm, 
                              uint64_t committed, const EntryVec& entries) {
  if (!matchTerm(index, logTerm)) {
    return 0;
  }

  uint64_t lastNewI, ci, offset;

  lastNewI = index + (uint64_t)entries.size();
  ci = findConflict(entries);

  if (ci == 0 || ci <= committed_) {
    logger_->Fatalf("entry %d conflict with committed entry [committed(%d)]", ci, committed_);
  }

  offset = index + 1;
  EntryVec appendEntries(entries.begin() + ci - offset, entries.end());
  append(appendEntries);
  commitTo(min(committed_, lastNewI));
  return lastNewI;
}

void raftLog::commitTo(uint64_t tocommit) {
  // never decrease commit
  if (committed_ >= tocommit) {
    return;
  }

  if (lastIndex() < tocommit) {
    logger_->Fatalf("tocommit(%d) is out of range [lastIndex(%d)]. Was the raft log corrupted, truncated, or lost?",
      tocommit, lastIndex());
  }

  committed_ = tocommit;
}

void raftLog::appliedTo(uint64_t i) {
  if (i == 0) {
    return;
  }

  if (committed_ < i || i < applied_) {
    logger_->Fatalf("applied(%d) is out of range [prevApplied(%d), committed(%d)]", i, applied_, committed_);
  }
  applied_ = i;
}

void raftLog::stableTo(uint64_t i, uint64_t t) {
  unstable_.stableTo(i, t);
}

void raftLog::stableSnapTo(uint64_t i) {
  unstable_.stableSnapTo(i);
}

uint64_t raftLog::lastTerm() {
  int err;
  uint64_t t;

  err = term(lastIndex(), &t);
  if (!SUCCESS(err)) {
    logger_->Fatalf("unexpected error when getting the last term (%s)", GetErrorString(err));
  }

  return t;
}

int raftLog::entries(uint64_t i, uint64_t maxSize, EntryVec *entries) {
  uint64_t lasti = lastIndex();

  if (i > lasti) {
    return OK;
  }

  return slice(i, lasti + 1, maxSize, entries);
}

// allEntries returns all entries in the log.
void raftLog::allEntries(EntryVec *entries) {
  int err = this->entries(firstIndex(), noLimit, entries);
  if (SUCCESS(err)) {
    return;
  }

  if (err == ErrCompacted) {
    return allEntries(entries);
  }
  logger_->Fatalf("allEntries fatal: %s", GetErrorString(err));
}

bool raftLog::isUpToDate(uint64_t lasti, uint64_t term) {
  uint64_t lastT = lastTerm();
  return term > lastT || (term == lastT && lasti >= lastIndex());
}

bool raftLog::maybeCommit(uint64_t maxIndex, uint64_t term) {
  uint64_t t;
  int err = this->term(maxIndex, &t);
  if (maxIndex > committed_ && zeroTermOnErrCompacted(t, err) == term) {
    commitTo(maxIndex);
    return true;
  }
  return false;
}

void raftLog::restore(Snapshot *snapshot) {
  logger_->Infof("log [%s] starts to restore snapshot [index: %d, term: %d]", 
    String().c_str(), snapshot->metadata().index(), snapshot->metadata().term());
  committed_ = snapshot->metadata().index();
  unstable_.restore(snapshot);
}

// append entries to unstable storage and return last index
// fatal if the first index of entries < committed_
uint64_t raftLog::append(const EntryVec& entries) {
  if (entries.size() == 0) {
    return lastIndex();
  }

  uint64_t after = entries[0].index() - 1;
  if (after < committed_) {
    logger_->Fatalf("after(%d) is out of range [committed(%d)]", after, committed_);
  }

  unstable_.truncateAndAppend(entries);
  return lastIndex();
}

// findConflict finds the index of the conflict.
// It returns the first pair of conflicting entries between the existing
// entries and the given entries, if there are any.
// If there is no conflicting entries, and the existing entries contains
// all the given entries, zero will be returned.
// If there is no conflicting entries, but the given entries contains new
// entries, the index of the first new entry will be returned.
// An entry is considered to be conflicting if it has the same index but
// a different term.
// The first entry MUST have an index equal to the argument 'from'.
// The index of the given entries MUST be continuously increasing.
uint64_t raftLog::findConflict(const EntryVec& entries) {
  int i;
  for (i = 0; i < entries.size(); ++i) {
    if (!matchTerm(entries[i].index(), entries[i].term())) {
      const Entry& entry = entries[i];
      uint64_t index = entry.index();
      uint64_t term = entry.term();

      if (index <= lastIndex()) {
        uint64_t dummy;
        int err = this->term(index, &dummy);
        logger_->Infof("found conflict at index %d [existing term: %d, conflicting term: %d]",
          index, zeroTermOnErrCompacted(dummy, err), term);
      }

      return index;
    }
  }

  return 0;
}

void raftLog::unstableEntries(EntryVec **entries) {
  if (unstable_.entries_.size() == 0) {
    *entries = NULL;
  }

  *entries = &unstable_.entries_;
}

// nextEntries returns all the available entries for execution.
// If applied is smaller than the index of snapshot, it returns all committed
// entries after the index of snapshot.
void raftLog::nextEntries(EntryVec* entries) {
  uint64_t offset = max(applied_ + 1, firstIndex());
  if (committed_ + 1 > offset) {
    int err = slice(offset, committed_ + 1, noLimit, entries);  
    if (!SUCCESS(err)) {
      logger_->Fatalf("unexpected error when getting unapplied entries (%s)", GetErrorString(err));
    }
  }
}

// hasNextEntries returns if there is any available entries for execution. This
// is a fast check without heavy raftLog.slice() in raftLog.nextEnts().
bool raftLog::hasNextEntries() {
  return committed_ + 1 > max(applied_ + 1, firstIndex());
}

int raftLog::snapshot(Snapshot **snapshot) {
  if (unstable_.snapshot_ != NULL) {
    *snapshot = unstable_.snapshot_;
    return OK;
  }

  return storage_->Snapshot(snapshot);
}

uint64_t raftLog::zeroTermOnErrCompacted(uint64_t t, int err) {
  if (SUCCESS(err)) {
    return t;
  }

  if (err == ErrCompacted) {
    return 0;
  }

  logger_->Fatalf("unexpected error: %s", GetErrorString(err));
  return 0;
}

bool raftLog::matchTerm(uint64_t i, uint64_t term) {
  int err;
  uint64_t t;

  err = this->term(i, &t);
  if (!SUCCESS(err)) {
    return false;
  }

  return t == term;
}

int raftLog::term(uint64_t i, uint64_t *t) {
  uint64_t dummyIndex;
  int err = OK;

  *t = 0;
  dummyIndex = firstIndex() - 1;
  if (i < dummyIndex || i > lastIndex()) {
    return OK;
  }

  *t = unstable_.maybeTerm(i);
  if (*t > 0) {
    goto out;
  }

  err = storage_->Term(i, t);
  if (SUCCESS(err)) {
    goto out;
  }

  if (err == ErrCompacted || err == ErrUnavailable) {
    goto out;
  }
  logger_->Fatalf("term err:%s", GetErrorString(err));
out:
  return err;
}

uint64_t raftLog::firstIndex() {
  uint64_t i;
  int err;

  i = unstable_.maybeFirstIndex();
  if (i) {
    return i;
  }

  err = storage_->FirstIndex(&i);
  if (!SUCCESS(err)) {
    logger_->Fatalf("firstIndex error:%s", GetErrorString(err));
  }

  return i;
}

uint64_t raftLog::lastIndex() {
  uint64_t i;
  int err;

  i = unstable_.maybeLastIndex();
  if (i > 0) {
    return i;
  }

  err = storage_->LastIndex(&i);
  if (!SUCCESS(err)) {
    logger_->Fatalf("lastIndex error:%s", GetErrorString(err));
  }

  return i;
}

// slice returns a slice of log entries from lo through hi-1, inclusive.
int raftLog::slice(uint64_t lo, uint64_t hi, uint64_t maxSize, EntryVec* entries) {
  int err;

  err = mustCheckOutOfBounds(lo, hi);
  if (!SUCCESS(err)) {
    return err;
  }

  if (lo == hi) {
    return OK;
  }

  if (lo < unstable_.offset_) {
    err = storage_->Entries(lo, hi, maxSize, entries);
    if (err == ErrCompacted) {
      return err;
    } else if (err == ErrUnavailable) {
      logger_->Fatalf("entries[%d:%d) is unavailable from storage", lo, min(hi, unstable_.offset_));
    } else if (!SUCCESS(err)) {
      logger_->Fatalf("storage entries err:%s", GetErrorString(err));
    }

    if ((uint64_t)entries->size() < min(hi, unstable_.offset_) - lo) {
      return OK;
    }
  }

  if (hi > unstable_.offset_) {
    EntryVec unstable;
    unstable_.slice(max(lo, unstable_.offset_), hi, &unstable);
    if (entries->size() > 0) {
      entries->insert(entries->end(), unstable.begin(), unstable.end());
    } else {
      *entries = unstable;
    }
  }

  limitSize(maxSize, entries); 
  return OK;
}

int raftLog::mustCheckOutOfBounds(uint64_t lo, uint64_t hi) {
  if (lo > hi) {
    logger_->Fatalf("invalid slice %d > %d", lo, hi);
  }

  uint64_t fi = firstIndex();
  if (lo < fi) {
    return ErrCompacted;
  }

  uint64_t li = lastIndex();
  if (hi > li + 1) {
    logger_->Fatalf("slice[%d,%d) out of bound [%d,%d]", lo, hi, fi, li);
  }

  return OK;
}