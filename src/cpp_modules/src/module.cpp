#include "main.cpp"
#include "types.hpp"
#include <nan.h>

using namespace v8;

NAN_METHOD(GetLockValueLookupDas) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  Nan::Utf8String utf8Value(inputStrNan);
  char const *inputStr = *utf8Value;

  // The engine throws std::runtime_error on internal invariant violations (e.g. the
  // positive-DAS-penalty guard in getLockValueLookupDas). Convert to a JS error so
  // the request fails loudly with the diagnostic message while the server survives.
  std::string result;
  try {
    result = mainProcess(inputStr, GET_LOCK_VALUE_LOOKUP_DAS);
  } catch (const std::exception &e) {
    Nan::ThrowError(e.what());
    return;
  }

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_METHOD(Precompute) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  Nan::Utf8String utf8Value(inputStrNan);
  char const *inputStr = *utf8Value;

  // The engine throws std::runtime_error on internal invariant violations.
  // Convert to a JS error so the request fails loudly with the diagnostic
  // message while the server survives.
  std::string result;
  try {
    result = mainProcess(inputStr, PRECOMPUTE);
  } catch (const std::exception &e) {
    Nan::ThrowError(e.what());
    return;
  }

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_METHOD(GetLockValueLookup) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  Nan::Utf8String utf8Value(inputStrNan);
  char const *inputStr = *utf8Value;

  std::string result = mainProcess(inputStr, GET_LOCK_VALUE_LOOKUP);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_METHOD(GetMove) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  Nan::Utf8String utf8Value(inputStrNan);
  char const *inputStr = *utf8Value;

  std::string result = mainProcess(inputStr, GET_MOVE);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_METHOD(GetMoveAbTest) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  Nan::Utf8String utf8Value(inputStrNan);
  char const *inputStr = *utf8Value;

  std::string result = mainProcess(inputStr, GET_MOVE_AB_TEST);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_METHOD(GetTopMoves) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  Nan::Utf8String utf8Value(inputStrNan);
  char const *inputStr = *utf8Value;

  std::string result = mainProcess(inputStr, GET_TOP_MOVES);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_METHOD(GetTopMovesHybrid) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  Nan::Utf8String utf8Value(inputStrNan);
  char const *inputStr = *utf8Value;

  std::string result = mainProcess(inputStr, GET_TOP_MOVES_HYBRID);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_METHOD(RateMove) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  Nan::Utf8String utf8Value(inputStrNan);
  char const *inputStr = *utf8Value;

  std::string result = mainProcess(inputStr, RATE_MOVE);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_MODULE_INIT(Init) {
  Nan::Set(target, Nan::New("precompute").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(Precompute)).ToLocalChecked());
  Nan::Set(target, Nan::New("getLockValueLookup").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(GetLockValueLookup)).ToLocalChecked());
  Nan::Set(target, Nan::New("getLockValueLookupDas").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(GetLockValueLookupDas)).ToLocalChecked());
  Nan::Set(target, Nan::New("getMove").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(GetMove)).ToLocalChecked());
  Nan::Set(target, Nan::New("getMoveAbTest").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(GetMoveAbTest)).ToLocalChecked());
  Nan::Set(target, Nan::New("getTopMoves").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(GetTopMoves)).ToLocalChecked());
  Nan::Set(target, Nan::New("getTopMovesHybrid").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(GetTopMovesHybrid)).ToLocalChecked());
  Nan::Set(target, Nan::New("rateMove").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(RateMove)).ToLocalChecked());
}

NODE_MODULE(myaddon, Init)
