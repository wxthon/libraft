#ifndef __LIB_RAFT_H__
#define __LIB_RAFT_H__

#include <cstdint>
#include <climits>
#include <string>
#include <vector>
#include "../src/raft.pb.h"

using namespace std;
using namespace raftpb;

const static uint64_t None = 0;
const static uint64_t noLimit = ULONG_MAX;

enum ErrorCode {
  OK                                = 0,
  ErrCompacted                      = 1,
  ErrSnapOutOfDate                  = 2,
  ErrUnavailable                    = 3,
  ErrSnapshotTemporarilyUnavailable = 4
};

#define SUCCESS(err) (err == OK)

enum RaftState {
  StateFollower = 1,
  StateCandidate = 2,
  StateLeader = 3,
  StatePreCandidate = 4
};

struct SoftState {
  uint64_t leader;
  RaftState state;
};

struct ReadState {
  uint64_t index;
  string   requestCtx;
};

typedef vector<Entry> EntryVec;

struct Ready {
  SoftState         softState;
  HardState         hardState;
  vector<ReadState> readStates;
  vector<Entry>     entries;
  vector<Entry>     committedEntries;
  vector<Message>   messages;
};

class Storage {
public:
  virtual int InitialState(HardState *, ConfState *) = 0;
  virtual int FirstIndex(uint64_t *index) = 0;
  virtual int LastIndex(uint64_t *index) = 0;
  virtual int Term(uint64_t i, uint64_t *term) = 0;
  virtual int Entries(uint64_t lo, uint64_t hi, uint64_t maxSize, vector<Entry> *entries) = 0;
  virtual int Snapshot(Snapshot **snapshot) = 0;
};

class Logger {
public:
  virtual void Debugf(const char *fmt, ...) = 0;
  virtual void Infof(const char *fmt, ...) = 0;
  virtual void Warningf(const char *fmt, ...) = 0;
  virtual void Errorf(const char *fmt, ...) = 0;
  virtual void Fatalf(const char *fmt, ...) = 0;
};

enum ReadOnlyOption {
  ReadOnlySafe        = 1,
  ReadOnlyLeaseBased  = 2
};

struct Config {
  uint64_t          id;
  vector<uint64_t>  peers;
  int               electionTick;
  int               heartbeatTick;
  Storage*          storage;
  uint64_t          applied;
  uint64_t          maxSizePerMsg;
  uint64_t          maxInflightMsgs;
  Logger*           logger;            
};

class Node {
public:
  /*
  virtual void Stop() = 0;
  virtual int Campaign(string data) = 0;
  virtual int Propose(string data) = 0;
  */
  virtual void Tick(Ready **ready) = 0;
};

extern Node* StartNode(Config *config);
extern Node* RestartNode(Config *config);
extern const char* GetErrorString(int err);

#endif  // __LIB_RAFT_H__