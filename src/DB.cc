#include "DB.h"
#include "Iterator.h"
#include "WriteBatch.h"

#include <node_buffer.h>
#include "helpers.h"

#include <stdlib.h>
#include <sstream>
#include <algorithm>

#define CHECK_VALID_STATE                                               \
  if (self->db == NULL) {                                               \
    return ThrowError("Illegal state: DB.open() has not been called");  \
  }

namespace node_leveldb {

Persistent<FunctionTemplate> DB::persistent_function_template;

DB::DB()
  : db(NULL)
{
}

DB::~DB() {
  // Close database and delete db
  Close();
}

void DB::Init(Handle<Object> target) {
  HandleScope scope; // used by v8 for garbage collection

  // Our constructor
  Local<FunctionTemplate> local_function_template = FunctionTemplate::New(New);
  persistent_function_template = Persistent<FunctionTemplate>::New(local_function_template);
  persistent_function_template->InstanceTemplate()->SetInternalFieldCount(1);
  persistent_function_template->SetClassName(String::NewSymbol("DB"));

  // Instance methods
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "open", Open);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "close", Close);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "put", Put);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "del", Del);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "write", Write);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "get", Get);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "newIterator", NewIterator);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "getSnapshot", GetSnapshot);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "releaseSnapshot", ReleaseSnapshot);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "getProperty", GetProperty);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "getApproximateSizes", GetApproximateSizes);
  NODE_SET_PROTOTYPE_METHOD(persistent_function_template, "compactRange", CompactRange);

  // Static methods
  NODE_SET_METHOD(persistent_function_template, "destroyDB", DestroyDB);
  NODE_SET_METHOD(persistent_function_template, "repairDB", RepairDB);

  // Binding our constructor function to the target variable
  target->Set(String::NewSymbol("DB"), persistent_function_template->GetFunction());

  // Set version
  std::stringstream version;
  version << leveldb::kMajorVersion << "." << leveldb::kMinorVersion;
  target->Set(String::New("bindingVersion"),
              String::New(version.str().c_str()),
              static_cast<v8::PropertyAttribute>(v8::ReadOnly|v8::DontDelete));
}

bool DB::HasInstance(Handle<Value> val) {
  if (!val->IsObject()) return false;
  Local<Object> obj = val->ToObject();

  if (persistent_function_template->HasInstance(obj))
    return true;

  return false;
}


//
// Constructor
//

Handle<Value> DB::New(const Arguments& args) {
  HandleScope scope;

  DB* self = new DB();
  self->Wrap(args.This());

  return args.This();
}


//
// Open
//

#define DB_OPEN_ARGS_ERROR(msg) \
  return ThrowTypeError(msg ": DB.open(<filename>, <options?>, <callback?>)")

Handle<Value> DB::Open(const Arguments& args) {
  HandleScope scope;

  // Get this and arguments
  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  if (argv < 1)
    DB_OPEN_ARGS_ERROR("Invalid number of arguments");

  if (!args[0]->IsString())
    DB_OPEN_ARGS_ERROR("Argument 1 must be a string");

  // Required filename
  String::Utf8Value name(args[0]);

  // Optional options
  leveldb::Options options = leveldb::Options();
  if (argv > 1 && args[1]->IsObject() && !args[1]->IsFunction())
    options = JsToOptions(args[1]);

  // Optional callback
  Local<Function> callback = GET_CALLBACK_ARG(args, argv);

  // Pass parameters to async function
  OpenParams *params = new OpenParams(self, *name, options, callback);
  EIO_BeforeOpen(params);

  return args.This();
}

void DB::EIO_BeforeOpen(OpenParams *params) {
  eio_custom(EIO_Open, EIO_PRI_DEFAULT, EIO_AfterOpen, params);
}

eio_return_type DB::EIO_Open(eio_req *req) {
  OpenParams *params = static_cast<OpenParams*>(req->data);
  DB *self = params->self;

  // Close old DB, if open() is called more than once
  self->Close();

  // Do the actual work
  params->status = leveldb::DB::Open(params->options, params->name, &self->db);

  eio_return_stmt;
}

int DB::EIO_AfterOpen(eio_req *req) {
  HandleScope scope;

  OpenParams *params = static_cast<OpenParams*>(req->data);
  params->Callback();

  delete params;
  return 0;
}


//
// Close
//

void DB::Close() {
  if (db != NULL) {
    // Close iterators because they must run their destructors before
    // we can delete the db object.
    std::vector< Persistent<Object> >::iterator it;
    for (it = iteratorList.begin(); it != iteratorList.end(); it++) {
      Iterator *itObj = ObjectWrap::Unwrap<Iterator>(*it);
      if (itObj) {
        itObj->Close();
      }
      it->Dispose();
      it->Clear();
    }
    iteratorList.clear();
    delete db;
    db = NULL;
  }
};

Handle<Value> DB::Close(const Arguments& args) {
  HandleScope scope;

  // Get this and arguments
  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  // Optional callback
  Local<Function> callback = GET_CALLBACK_ARG(args, argv);

  self->Close();

  Params *params = new Params(self, callback);
  EIO_BeforeClose(params);

  return args.This();
}

void DB::EIO_BeforeClose(Params *params) {
  eio_custom(EIO_Close, EIO_PRI_DEFAULT, EIO_AfterClose, params);
}

eio_return_type DB::EIO_Close(eio_req *req) {
  Params *params = static_cast<Params*>(req->data);
  DB *self = params->self;

  eio_return_stmt;
}

int DB::EIO_AfterClose(eio_req *req) {
  Params *params = static_cast<Params*>(req->data);
  params->Callback();

  delete params;
  return 0;
}


//
// Put
//

#define DB_PUT_ARGS_ERROR(msg) \
  return ThrowTypeError(msg ": DB.put(<key>, <value>, <options?>, <callback?>)")

Handle<Value> DB::Put(const Arguments& args) {
  HandleScope scope;

  // Get this and arguments
  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  CHECK_VALID_STATE;

  if (argv < 2)
    DB_PUT_ARGS_ERROR("Invalid number of arguments");

  if (!args[0]->IsString() && !Buffer::HasInstance(args[0]))
    DB_OPEN_ARGS_ERROR("Argument 1 must be a string or buffer");

  if (!args[1]->IsString() && !Buffer::HasInstance(args[1]))
    DB_OPEN_ARGS_ERROR("Argument 2 must be a string or buffer");

  // Use temporary WriteBatch to implement Put
  WriteBatch *writeBatch = new WriteBatch();
  leveldb::Slice key = JsToSlice(args[0], &writeBatch->strings);
  leveldb::Slice value = JsToSlice(args[1], &writeBatch->strings);
  writeBatch->wb.Put(key, value);

  // Optional write options
  leveldb::WriteOptions options = leveldb::WriteOptions();
  if (argv > 2 && args[2]->IsObject() && !args[2]->IsFunction())
    options = JsToWriteOptions(args[2]);

  // Optional callback
  Local<Function> callback = GET_CALLBACK_ARG(args, argv);

  WriteParams *params = new WriteParams(self, writeBatch, options, callback);
  params->disposeWriteBatch = true;
  EIO_BeforeWrite(params);

  return args.This();
}

#undef DB_PUT_ARGS_ERROR


//
// Del
//

#define DB_DEL_ARGS_ERROR(msg) \
  return ThrowTypeError(msg ": DB.del(<key>, <options?>, <callback?>)")

Handle<Value> DB::Del(const Arguments& args) {
  HandleScope scope;

  // Get this and arguments
  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  CHECK_VALID_STATE;

  if (argv < 1)
    DB_DEL_ARGS_ERROR("Invalid number of arguments");

  // Use temporary WriteBatch to implement Del
  WriteBatch *writeBatch = new WriteBatch();
  leveldb::Slice key = JsToSlice(args[0], &writeBatch->strings);
  writeBatch->wb.Delete(key);

  // Optional write options
  leveldb::WriteOptions options = GET_WRITE_OPTIONS_ARG(args, argv, 1);

  // Optional callback
  Local<Function> callback = GET_CALLBACK_ARG(args, argv);

  WriteParams *params = new WriteParams(self, writeBatch, options, callback);
  params->disposeWriteBatch = true;
  EIO_BeforeWrite(params);

  return args.This();
}

#undef DB_DEL_ARGS_ERROR


//
// Write
//

#define DB_WRITE_ARGS_ERROR(msg) \
  return ThrowTypeError(msg ": DB.write(<key>, <options?>, <callback?>)")

Handle<Value> DB::Write(const Arguments& args) {
  HandleScope scope;

  // Get this and arguments
  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  CHECK_VALID_STATE;

  if (argv < 1)
    DB_WRITE_ARGS_ERROR("Invalid number of arguments");

  // Required WriteBatch
  if (!args[0]->IsObject())
    DB_WRITE_ARGS_ERROR("Argument 1 must be a WriteBatch object");

  Local<Object> writeBatchObject = Object::Cast(*args[0]);
  WriteBatch* writeBatch = ObjectWrap::Unwrap<WriteBatch>(writeBatchObject);

  if (writeBatch == NULL)
    DB_WRITE_ARGS_ERROR("Argument 1 must be a WriteBatch object");

  // Optional write options
  leveldb::WriteOptions options = GET_WRITE_OPTIONS_ARG(args, argv, 2);

  // Optional callback
  Local<Function> callback = GET_CALLBACK_ARG(args, argv);

  // Pass parameters to async function
  WriteParams *params = new WriteParams(self, writeBatch, options, callback);

  if (!params->disposeWriteBatch) writeBatch->Ref();

  EIO_BeforeWrite(params);

  return args.This();
}

#undef DB_WRITE_ARGS_ERROR

void DB::EIO_BeforeWrite(WriteParams *params) {
  eio_custom(EIO_Write, EIO_PRI_DEFAULT, EIO_AfterWrite, params);
}

eio_return_type DB::EIO_Write(eio_req *req) {
  WriteParams *params = static_cast<WriteParams*>(req->data);
  DB *self = params->self;

  // Do the actual work
  if (self->db != NULL) {
    params->status = self->db->Write(params->options, &params->writeBatch->wb);
  }

  eio_return_stmt;
}

int DB::EIO_AfterWrite(eio_req *req) {
  HandleScope scope;

  WriteParams *params = static_cast<WriteParams*>(req->data);
  params->Callback();

  if (params->disposeWriteBatch) {
    delete params->writeBatch;
  } else {
    params->writeBatch->Unref();
  }

  delete params;
  return 0;
}


//
// Get
//

#define DB_GET_ARGS_ERROR(msg) \
  return ThrowTypeError(msg ": DB.get(<key>, <options?>, <callback?>)")

Handle<Value> DB::Get(const Arguments& args) {
  HandleScope scope;

  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  CHECK_VALID_STATE;

  if (argv < 1)
    DB_GET_ARGS_ERROR("Invalid number of arguments");

  if (!args[0]->IsString() && !Buffer::HasInstance(args[0]))
    DB_GET_ARGS_ERROR("Argument 1 must be a string or buffer");

  bool asBuffer = false;

  // Optional read options
  leveldb::ReadOptions options = GET_READ_OPTIONS_ARG(asBuffer, args, argv, 1);

  // Optional callback
  Local<Function> callback = GET_CALLBACK_ARG(args, argv);

  // Pass parameters to async function
  ReadParams *params = new ReadParams(self, options, asBuffer, callback);

  // Set key parameter
  if (args[0]->IsString()) {
    String::Utf8Value str(args[0]);
    params->keyLen = str.length();
    char *tmp = (char*)malloc(params->keyLen);
    memcpy(tmp, *str, params->keyLen);
    params->key = tmp;
    params->keyBuf = Persistent<Object>();
  } else {
    Handle<Object> buf = args[0]->ToObject();
    params->key = Buffer::Data(buf);
    params->keyLen = Buffer::Length(buf);
    params->keyBuf = Persistent<Object>::New(buf);
  }
  EIO_BeforeRead(params);

  return args.This();
}

#undef DB_GET_ARGS_ERROR

void DB::EIO_BeforeRead(ReadParams *params) {
  eio_custom(EIO_Read, EIO_PRI_DEFAULT, EIO_AfterRead, params);
}

eio_return_type DB::EIO_Read(eio_req *req) {
  ReadParams *params = static_cast<ReadParams*>(req->data);
  DB *self = params->self;

  leveldb::Slice key(params->key, params->keyLen);

  // Do the actual work
  if (self->db != NULL) {
    params->status = self->db->Get(params->options, key, &params->result);
  }

  eio_return_stmt;
}

int DB::EIO_AfterRead(eio_req *req) {
  HandleScope scope;

  ReadParams *params = static_cast<ReadParams*>(req->data);
  if (params->asBuffer) {
    params->Callback(Bufferize(params->result));
  } else {
    params->Callback(String::New(params->result.data(), params->result.length()));
  }

  if (!params->keyBuf.IsEmpty()) {
    params->keyBuf.Dispose();
  } else {
    free(params->key);
  }

  delete params;
  return 0;
}


//
// NewIterator
//

Handle<Value> DB::NewIterator(const Arguments& args) {
  HandleScope scope;

  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  CHECK_VALID_STATE;

  bool asBuffer = false;

  leveldb::ReadOptions options = GET_READ_OPTIONS_ARG(asBuffer, args, argv, 0);
  leveldb::Iterator* it = self->db->NewIterator(options);

  // https://github.com/joyent/node/blob/master/deps/v8/include/v8.h#L2102
  Local<Value> itArgs[] = {External::New(it), args.This()};
  Handle<Object> itHandle = Iterator::persistent_function_template->GetFunction()->NewInstance(2, itArgs);

  // Keep a weak reference
  Persistent<Object> weakHandle = Persistent<Object>::New(itHandle);
  weakHandle.MakeWeak(&self->iteratorList, &DB::unrefIterator);
  self->iteratorList.push_back(weakHandle);

  return scope.Close(itHandle);
}

bool CheckAlive(Persistent<Object> o) {
  return !o.IsNearDeath();
};

void DB::unrefIterator(Persistent<Value> object, void* parameter) {
  std::vector< Persistent<Object> > *iteratorList =
    (std::vector< Persistent<Object> > *) parameter;

  Iterator *itTarget = ObjectWrap::Unwrap<Iterator>(object->ToObject());

  std::vector< Persistent<Object> >::iterator i =
    partition(iteratorList->begin(), iteratorList->end(), CheckAlive);

  iteratorList->erase(i, iteratorList->end());
}


//
// GetSnapshot
//

Handle<Value> DB::GetSnapshot(const Arguments& args) {
  HandleScope scope;
  return ThrowError("Method not implemented");
}


//
// ReleaseSnapshot
//

Handle<Value> DB::ReleaseSnapshot(const Arguments& args) {
  HandleScope scope;
  return ThrowError("Method not implemented");
}


//
// GetProperty
//

Handle<Value> DB::GetProperty(const Arguments& args) {
  HandleScope scope;
  return ThrowError("Method not implemented");
}


//
// GetApproximateSizes
//

Handle<Value> DB::GetApproximateSizes(const Arguments& args) {
  HandleScope scope;
  return ThrowError("Method not implemented");
}


//
// CompactRange
//

Handle<Value> DB::CompactRange(const Arguments& args) {
  HandleScope scope;
  return ThrowError("Method not implemented");
}


//
// DestroyDB
//

#define DB_DESTROY_DB_ARGS_ERROR(msg) \
  return ThrowTypeError(msg ": DB.destroyDB(<filename>, <options?>)")

Handle<Value> DB::DestroyDB(const Arguments& args) {
  HandleScope scope;

  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  CHECK_VALID_STATE;

  if (argv < 1)
    DB_DESTROY_DB_ARGS_ERROR("Invalid number of arguments");

  // Check args
  if (!args[0]->IsString())
    DB_DESTROY_DB_ARGS_ERROR("Argument 1 must be a string");

  String::Utf8Value name(args[0]);
  leveldb::Options options = GET_OPTIONS_ARG(args, argv, 1);

  return processStatus(leveldb::DestroyDB(*name, options));
}

#undef DB_DESTROY_DB_ARGS_ERROR


//
// RepairDB
//

#define DB_REPAIR_DB_ARGS_ERROR(msg) \
  return ThrowTypeError(msg ": DB.repairDB(<filename>, <options?>)")

Handle<Value> DB::RepairDB(const Arguments& args) {
  HandleScope scope;

  DB* self = ObjectWrap::Unwrap<DB>(args.This());
  int argv = args.Length();

  CHECK_VALID_STATE;

  if (argv < 1)
    DB_REPAIR_DB_ARGS_ERROR("Invalid number of arguments");

  // Check args
  if (!args[0]->IsString())
    DB_REPAIR_DB_ARGS_ERROR("Argument 1 must be a string");

  String::Utf8Value name(args[0]);
  leveldb::Options options = GET_OPTIONS_ARG(args, argv, 1);

  return processStatus(leveldb::RepairDB(*name, options));
}


//
// Implementation of Params, which are passed from JS thread to EIO thread and back again
//

DB::Params::Params(DB* self, Handle<Function> cb)
  : self(self)
{
  self->Ref();
  ev_ref(EV_DEFAULT_UC);
  callback = Persistent<Function>::New(cb);
}

DB::Params::~Params() {
  self->Unref();
  ev_unref(EV_DEFAULT_UC);
  callback.Dispose();
}

void DB::Params::Callback(Handle<Value> result) {
  if (!callback.IsEmpty()) {
    TryCatch try_catch;
    if (status.ok()) {
      // no error, callback with no arguments, or result as second argument
      if (result.IsEmpty()) {
        callback->Call(self->handle_, 0, NULL);
      } else {
        Handle<Value> argv[] = { Null(), result };
        callback->Call(self->handle_, 2, argv);
      }
    } else if (status.IsNotFound()) {
      // not found, return (null, undef)
      Handle<Value> argv[] = { Null() };
      callback->Call(self->handle_, 1, argv);
    } else {
      // error, callback with first argument as error object
      Handle<String> message = String::New(status.ToString().c_str());
      Handle<Value> argv[] = { Exception::Error(message) };
      callback->Call(self->handle_, 1, argv);
    }
    if (try_catch.HasCaught()) {
        FatalException(try_catch);
    }
  }
}

} // namespace node_leveldb
