#include <nan.h>
#include "main.cpp"
#include "types.hpp"

using namespace v8;

NAN_METHOD(GetLockValueLookup) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  char const * inputStr = *Nan::Utf8String(inputStrNan);

  std::string result = mainProcess(inputStr, GET_LOCK_VALUE_LOOKUP);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_METHOD(PlayMoveNoNextBox) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  char const * inputStr = *Nan::Utf8String(inputStrNan);

  std::string result = mainProcess(inputStr, PLAY_MOVE_NO_NEXT_BOX);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_MODULE_INIT(Init) {
  Nan::Set(target, Nan::New("getLockValueLookup").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(GetLockValueLookup)).ToLocalChecked());
  Nan::Set(target, Nan::New("playMoveNoNextBox").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(PlayMoveNoNextBox)).ToLocalChecked());
}

NODE_MODULE(myaddon, Init)
