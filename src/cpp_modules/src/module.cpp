#include <nan.h>
#include "main.cpp"

using namespace v8;

NAN_METHOD(Length) {
  // Parse string arg
  Nan::MaybeLocal<String> maybeStr = Nan::To<String>(info[0]);
  v8::Local<String> inputStrNan;
  if (maybeStr.ToLocal(&inputStrNan) == false) {
    Nan::ThrowError("Error converting first argument to string");
  }
  char const * inputStr = *Nan::Utf8String(inputStrNan);

  std::string result = mainProcess(inputStr, /* isDebug= */ false);

  info.GetReturnValue().Set(Nan::New<String>(result.c_str()).ToLocalChecked());
}

NAN_MODULE_INIT(Init) {
  Nan::Set(target, Nan::New("length").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(Length)).ToLocalChecked());
}

NODE_MODULE(myaddon, Init)
