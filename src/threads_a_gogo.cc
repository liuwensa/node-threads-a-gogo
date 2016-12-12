//2011-11 Proyectos Equis Ka, s.l., jorge@jorgechamorro.com
//threads_a_gogo.cc

#include <v8.h>
#include <node.h>
#include <node_version.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string>

#define kThreadMagicCookie 0x99c0ffee
#define TAGG_USE_LIBUV
#if (NODE_MAJOR_VERSION == 0) && (NODE_MINOR_VERSION <= 5)
#undef TAGG_USE_LIBUV
#endif

//using namespace node;
using namespace v8;

enum jobTypes {
  kJobTypeEval,
  kJobTypeEvent
};

struct typeEvent {
  int length;
  String::Utf8Value* eventName;
  String::Utf8Value** argumentos;
};

struct typeEval {
  int error;
  int tiene_callBack;
  int useStringObject;
  String::Utf8Value* resultado;
  union {
    char* scriptText_CharPtr;
    String::Utf8Value* scriptText_StringObject;
  };
};
     
struct typeJob {
  int jobType;
  Persistent<Object> cb;
  union {
    typeEval eval;
    typeEvent event;
  };
};

struct typeQueueItem {
  typeQueueItem* next;
  typeJob job;
};

struct typeQueue {
  typeQueueItem* last;
  typeQueueItem* first;
  volatile long int length;
  pthread_mutex_t queueLock;
};

struct typeThread {

#ifdef TAGG_USE_LIBUV
  uv_async_t async_watcher; //MUST be the first one
#else
  ev_async async_watcher; //MUST be the first one
#endif
  
  long int id;
  pthread_t thread;
  volatile int sigkill;
  
  typeQueue toThreadQueue;  //Jobs to run
  typeQueue toProcessQueue; //Jobs done
  
  volatile int IDLE;
  pthread_cond_t IDLE_cv;
  pthread_mutex_t IDLE_mutex;
  
  Isolate* isolate;
  Persistent<Context> context;
  Persistent<Object> JSObject;
  Persistent<Object> threadJSObject;
  Persistent<Object> dispatchEvents;
  
  unsigned long threadMagicCookie;
};

#include "queues_a_gogo.cc"

static bool useLocker;
static long int threadsCtr= 0;
static Persistent<String> id_symbol;
static typeQueue* freeJobsQueue= NULL;
static Persistent<ObjectTemplate> threadTemplate;


/*

cd deps/minifier/src
gcc minify.c -o minify
cat ../../../src/events.js | ./minify kEvents_js > ../../../src/kEvents_js
cat ../../../src/load.js | ./minify kLoad_js > ../../../src/kLoad_js
cat ../../../src/createPool.js | ./minify kCreatePool_js > ../../../src/kCreatePool_js
cat ../../../src/thread_nextTick.js | ./minify kThread_nextTick_js > ../../../src/kThread_nextTick_js

*/

#include "events.js.c"
#include "createPool.js.c"
#include "thread_nextTick.js.c"

//node-waf configure uninstall distclean configure build install








static typeQueueItem* nuJobQueueItem (void) {
  typeQueueItem* qitem= qPull(freeJobsQueue);
  if (!qitem) qitem= nuQitem();
  return qitem;
}






static typeThread* isAThread (Handle<Object> receiver) {
  typeThread* thread;
  if (receiver->IsObject()) {
    if (receiver->InternalFieldCount() == 1) {
      thread= (typeThread*) receiver->GetPointerFromInternalField(0);
      if (thread && (thread->threadMagicCookie == kThreadMagicCookie)) {
        return thread;
      }
    }
  }
  return NULL;
}






static void pushToInQueue (typeQueueItem* qitem, typeThread* thread) {
  pthread_mutex_lock(&thread->IDLE_mutex);
  qPush(qitem, &thread->toThreadQueue);
  if (thread->IDLE) {
    pthread_cond_signal(&thread->IDLE_cv);
  }
  pthread_mutex_unlock(&thread->IDLE_mutex);
}






static Handle<Value> Puts (const Arguments &args) {
  HandleScope scope;
  int i= 0;
  while (i < args.Length()) {
    String::Utf8Value c_str(args[i]);
    fputs(*c_str, stdout);
    i++;
  }
  fflush(stdout);
  return Undefined();
}





static void eventLoop (typeThread* thread);

//Esto es lo primero que ejecuta una thread al nacer.
static void* threadBootProc (void* arg) {
  
  int dummy;
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &dummy);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &dummy);
  
  typeThread* thread= (typeThread*) arg;
  thread->isolate= Isolate::New();
  thread->isolate->SetData(thread);
  
  if (useLocker) {
    //printf("**** USING LOCKER: YES\n");
    v8::Locker myLocker(thread->isolate);
    //v8::Isolate::Scope isolate_scope(thread->isolate);
    eventLoop(thread);
  }
  else {
    //printf("**** USING LOCKER: NO\n");
    //v8::Isolate::Scope isolate_scope(thread->isolate);
    eventLoop(thread);
  }
  thread->isolate->Exit();
  thread->isolate->Dispose();
  
  // wake up callback
#ifdef TAGG_USE_LIBUV
  uv_async_send(&thread->async_watcher);
#else
  ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher);
#endif
  
  return NULL;
}





static Handle<Value> threadEmit (const Arguments &args);

static void eventLoop (typeThread* thread) {
  thread->isolate->Enter();
  thread->context= Context::New();
  thread->context->Enter();
  
  {
    HandleScope scope1;
    
    Local<Object> global= thread->context->Global();
    global->Set(String::NewSymbol("puts"), FunctionTemplate::New(Puts)->GetFunction());
    Local<Object> threadObject= Object::New();
    global->Set(String::NewSymbol("thread"), threadObject);
    
    threadObject->Set(String::NewSymbol("id"), Number::New(thread->id));
    threadObject->Set(String::NewSymbol("emit"), FunctionTemplate::New(threadEmit)->GetFunction());
    Local<Object> dispatchEvents= Script::Compile(String::New(kEvents_js))->Run()->ToObject()->CallAsFunction(threadObject, 0, NULL)->ToObject();
    Local<Object> dispatchNextTicks= Script::Compile(String::New(kThread_nextTick_js))->Run()->ToObject();
    Local<Array> _ntq= (v8::Array*) *threadObject->Get(String::NewSymbol("_ntq"));
    
    double nextTickQueueLength= 0;
    long int ctr= 0;
    
    //SetFatalErrorHandler(FatalErrorCB);
    
    while (!thread->sigkill) {
      typeJob* job;
      typeQueueItem* qitem;
      
      {
        HandleScope scope2;
        TryCatch onError;
        String::Utf8Value* str;
        Local<String> source;
        Local<Script> script;
        Local<Value> resultado;
        
        
        while ((qitem= qPull(&thread->toThreadQueue))) {
          
          job= &qitem->job;
          
          if ((++ctr) > 2e3) {
            ctr= 0;
            V8::IdleNotification();
          }
          
          if (job->jobType == kJobTypeEval) {
            //Ejecutar un texto
            
            if (job->eval.useStringObject) {
              str= job->eval.scriptText_StringObject;
              source= String::New(**str, (*str).length());
              delete str;
            }
            else {
              source= String::New(job->eval.scriptText_CharPtr);
              free(job->eval.scriptText_CharPtr);
            }
            
            script= Script::New(source);
            
            if (!onError.HasCaught()) resultado= script->Run();

            if (job->eval.tiene_callBack) {
              job->eval.error= onError.HasCaught() ? 1 : 0;
              job->eval.resultado= new String::Utf8Value(job->eval.error ? onError.Exception() : resultado);
              qPush(qitem, &thread->toProcessQueue);

// wake up callback
#ifdef TAGG_USE_LIBUV
              uv_async_send(&thread->async_watcher);
#else
              ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher);
#endif

            }
            else {
              qPush(qitem, freeJobsQueue);
            }

            if (onError.HasCaught()) onError.Reset();
          }
          else if (job->jobType == kJobTypeEvent) {
            //Emitir evento.
            
            Local<Value> args[2];
            str= job->event.eventName;
            args[0]= String::New(**str, (*str).length());
            delete str;
            
            Local<Array> array= Array::New(job->event.length);
            args[1]= array;
            
            int i= 0;
            while (i < job->event.length) {
              str= job->event.argumentos[i];
              array->Set(i, String::New(**str, (*str).length()));
              delete str;
              i++;
            }
            
            free(job->event.argumentos);
            qPush(qitem, freeJobsQueue);
            dispatchEvents->CallAsFunction(global, 2, args);
          }
        }
        
        if (_ntq->Length()) {
          
          if ((++ctr) > 2e3) {
            ctr= 0;
            V8::IdleNotification();
          }
          
          resultado= dispatchNextTicks->CallAsFunction(global, 0, NULL);
          if (onError.HasCaught()) {
            nextTickQueueLength= 1;
            onError.Reset();
          }
          else {
            nextTickQueueLength= resultado->NumberValue();
          }
        }
      }
      
      if (nextTickQueueLength || thread->toThreadQueue.length) continue;
      if (thread->sigkill) break;
      
      pthread_mutex_lock(&thread->IDLE_mutex);
      if (!thread->toThreadQueue.length) {
        thread->IDLE= 1;
        pthread_cond_wait(&thread->IDLE_cv, &thread->IDLE_mutex);
        thread->IDLE= 0;
      }
      pthread_mutex_unlock(&thread->IDLE_mutex);
    }
  }
  
  thread->context.Dispose();
}






static void destroyaThread (typeThread* thread) {
  
  thread->sigkill= 0;
  //TODO: hay que vaciar las colas y destruir los trabajos antes de ponerlas a NULL
  thread->toThreadQueue.first= thread->toThreadQueue.last= NULL;
  thread->toProcessQueue.first= thread->toProcessQueue.last= NULL;
  thread->JSObject->SetPointerInInternalField(0, NULL);
  thread->JSObject.Dispose();
  
#ifdef TAGG_USE_LIBUV
  uv_close((uv_handle_t*) &thread->async_watcher, NULL);
  //uv_unref(&thread->async_watcher);
#else
  ev_async_stop(EV_DEFAULT_UC_ &thread->async_watcher);
  ev_unref(EV_DEFAULT_UC);
#endif
  
  free(thread);
}






// C callback that runs in the main nodejs thread. This is the one responsible for
// calling the thread's JS callback.
static void Callback (
#ifdef TAGG_USE_LIBUV
  uv_async_t *watcher
#else
  EV_P_ ev_async *watcher
#endif
, int revents) {
  typeThread* thread= (typeThread*) watcher;
  
  if (thread->sigkill) {
    destroyaThread(thread);
    return;
  }
  
  HandleScope scope;
  typeJob* job;
  Local<Value> argv[2];
  Local<Value> null= Local<Value>::New(Null());
  typeQueueItem* qitem;
  String::Utf8Value* str;
  
  TryCatch onError;
  while ((qitem= qPull(&thread->toProcessQueue))) {
    job= &qitem->job;

    if (job->jobType == kJobTypeEval) {

      if (job->eval.tiene_callBack) {
        str= job->eval.resultado;

        if (job->eval.error) {
          argv[0]= Exception::Error(String::New(**str, (*str).length()));
          argv[1]= null;
        } else {
          argv[0]= null;
          argv[1]= String::New(**str, (*str).length());
        }
        job->cb->CallAsFunction(thread->JSObject, 2, argv);
        job->cb.Dispose();
        job->eval.tiene_callBack= 0;

        delete str;
        job->eval.resultado= NULL;
      }

      qPush(qitem, freeJobsQueue);
      
      if (onError.HasCaught()) {
        if (thread->toProcessQueue.first) {
        
#ifdef TAGG_USE_LIBUV
          uv_async_send(&thread->async_watcher); // wake up callback again
#else
          ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher); // wake up callback again
#endif
        }
        node::FatalException(onError);
        return;
      }
    }
    else if (job->jobType == kJobTypeEvent) {
      
      //fprintf(stdout, "*** Callback\n");
      
      Local<Value> args[2];
      
      str= job->event.eventName;
      args[0]= String::New(**str, (*str).length());
      delete str;
      
      Local<Array> array= Array::New(job->event.length);
      args[1]= array;
      
      int i= 0;
      while (i < job->event.length) {
        str= job->event.argumentos[i];
        array->Set(i, String::New(**str, (*str).length()));
        delete str;
        i++;
      }
      
      free(job->event.argumentos);
      qPush(qitem, freeJobsQueue);
      thread->dispatchEvents->CallAsFunction(thread->JSObject, 2, args);
    }
  }
}






// unconditionally destroys a thread by brute force.
static Handle<Value> Destroy (const Arguments &args) {
  HandleScope scope;
  //TODO: Hay que comprobar que this en un objeto y que tiene hiddenRefTotypeThread_symbol y que no es nil
  //TODO: Aquí habría que usar static void TerminateExecution(int thread_id);
  //TODO: static void v8::V8::TerminateExecution  ( Isolate *   isolate= NULL   )   [static]
  
  typeThread* thread= isAThread(args.This());
  if (!thread) {
    return ThrowException(Exception::TypeError(String::New("thread.destroy(): the receiver must be a thread object")));
  }
  
  if (!thread->sigkill) {
    //pthread_cancel(thread->thread);
    thread->sigkill= 1;
    pthread_mutex_lock(&thread->IDLE_mutex);
    if (thread->IDLE) {
      pthread_cond_signal(&thread->IDLE_cv);
    }
    pthread_mutex_unlock(&thread->IDLE_mutex);
  }
  
  return Undefined();
}






// Eval: Pushes a job into the thread's ->toThreadQueue.
static Handle<Value> Eval (const Arguments &args) {
  HandleScope scope;
  
  if (!args.Length()) {
    return ThrowException(Exception::TypeError(String::New("thread.eval(program [,callback]): missing arguments")));
  }
  
  typeThread* thread= isAThread(args.This());
  if (!thread) {
    return ThrowException(Exception::TypeError(String::New("thread.eval(): the receiver must be a thread object")));
  }

  typeQueueItem* qitem= nuJobQueueItem();
  typeJob* job= &qitem->job;
  
  job->eval.tiene_callBack= ((args.Length() > 1) && (args[1]->IsFunction()));
  if (job->eval.tiene_callBack) {
    job->cb= Persistent<Object>::New(args[1]->ToObject());
  }
  job->eval.scriptText_StringObject= new String::Utf8Value(args[0]);
  job->eval.useStringObject= 1;
  job->jobType= kJobTypeEval;
  
  pushToInQueue(qitem, thread);
  return scope.Close(args.This());
}





static char* readFile (Handle<String> path) {
  v8::String::Utf8Value c_str(path);
  FILE* fp= fopen(*c_str, "rb");
  if (!fp) {
    fprintf(stderr, "Error opening the file %s\n", *c_str);
    //@bruno: Shouldn't we throw, here ?
    return NULL;
  }
  fseek(fp, 0, SEEK_END);
  long len= ftell(fp);
  rewind(fp); //fseek(fp, 0, SEEK_SET);
  char *buf= (char*) calloc(len + 1, sizeof(char)); // +1 to get null terminated string
  fread(buf, len, 1, fp);
  fclose(fp);
  /*
  printf("SOURCE:\n%s\n", buf);
  fflush(stdout);
  */
  return buf;
}






// Load: Loads from file and passes to Eval
static Handle<Value> Load (const Arguments &args) {
  HandleScope scope;

  if (!args.Length()) {
    return ThrowException(Exception::TypeError(String::New("thread.load(filename [,callback]): missing arguments")));
  }

  typeThread* thread= isAThread(args.This());
  if (!thread) {
    return ThrowException(Exception::TypeError(String::New("thread.load(): the receiver must be a thread object")));
  }
  
  char* source= readFile(args[0]->ToString());  //@Bruno: here we don't know if the file was not found or if it was an empty file
  if (!source) return scope.Close(args.This()); //@Bruno: even if source is empty, we should call the callback ?

  typeQueueItem* qitem= nuJobQueueItem();
  typeJob* job= &qitem->job;

  job->eval.tiene_callBack= ((args.Length() > 1) && (args[1]->IsFunction()));
  if (job->eval.tiene_callBack) {
    job->cb= Persistent<Object>::New(args[1]->ToObject());
  }
  job->eval.scriptText_CharPtr= source;
  job->eval.useStringObject= 0;
  job->jobType= kJobTypeEval;

  pushToInQueue(qitem, thread);

  return scope.Close(args.This());
}






static Handle<Value> processEmit (const Arguments &args) {
  HandleScope scope;
  
  if (!args.Length()) return scope.Close(args.This());
  
  typeThread* thread= isAThread(args.This());
  if (!thread) {
    return ThrowException(Exception::TypeError(String::New("thread.emit(): the receiver must be a thread object")));
  }
  
  typeQueueItem* qitem= nuJobQueueItem();
  typeJob* job= &qitem->job;
  
  job->jobType= kJobTypeEvent;
  job->event.length= args.Length()- 1;
  job->event.eventName= new String::Utf8Value(args[0]);
  job->event.argumentos= (v8::String::Utf8Value**) malloc(job->event.length* sizeof(void*));
  
  int i= 1;
  do {
    job->event.argumentos[i-1]= new String::Utf8Value(args[i]);
  } while (++i <= job->event.length);
  
  pushToInQueue(qitem, thread);
  
  return scope.Close(args.This());
}






static Handle<Value> threadEmit (const Arguments &args) {
  HandleScope scope;
  
  if (!args.Length()) return scope.Close(args.This());
  
  typeThread* thread= (typeThread*) Isolate::GetCurrent()->GetData();
  typeQueueItem* qitem= nuJobQueueItem();
  typeJob* job= &qitem->job;
  
  job->jobType= kJobTypeEvent;
  job->event.length= args.Length()- 1;
  job->event.eventName= new String::Utf8Value(args[0]);
  job->event.argumentos= (v8::String::Utf8Value**) malloc(job->event.length* sizeof(void*));
  
  int i= 1;
  do {
    job->event.argumentos[i-1]= new String::Utf8Value(args[i]);
  } while (++i <= job->event.length);
  
  qPush(qitem, &thread->toProcessQueue);
  
#ifdef TAGG_USE_LIBUV
  uv_async_send(&thread->async_watcher); // wake up callback
#else
  ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher); // wake up callback
#endif
  
  return scope.Close(args.This());
}








// Creates and launches a new isolate in a new background thread.
static Handle<Value> Create (const Arguments &args) {
    HandleScope scope;
    
    typeThread* thread= (typeThread*) calloc(1, sizeof (typeThread));
    thread->id= threadsCtr++;
    thread->threadMagicCookie= kThreadMagicCookie;
    thread->JSObject= Persistent<Object>::New(threadTemplate->NewInstance());
    thread->JSObject->SetPointerInInternalField(0, thread);
    thread->JSObject->Set(id_symbol, Integer::New(thread->id));
    Local<Value> dispatchEvents= Script::Compile(String::New(kEvents_js))->Run()->ToObject()->CallAsFunction(thread->JSObject, 0, NULL);
    thread->dispatchEvents= Persistent<Object>::New(dispatchEvents->ToObject());
    
#ifdef TAGG_USE_LIBUV
    uv_async_init(uv_default_loop(), &thread->async_watcher, Callback);
#else
    ev_async_init(&thread->async_watcher, Callback);
    ev_async_start(EV_DEFAULT_UC_ &thread->async_watcher);
    ev_ref(EV_DEFAULT_UC);
#endif
    
    pthread_cond_init(&(thread->IDLE_cv), NULL);
    pthread_mutex_init(&(thread->IDLE_mutex), NULL);
    pthread_mutex_init(&(thread->toThreadQueue.queueLock), NULL);
    pthread_mutex_init(&(thread->toProcessQueue.queueLock), NULL);
    if (pthread_create(&(thread->thread), NULL, threadBootProc, thread)) {
      pthread_cond_destroy(&(thread->IDLE_cv));
      pthread_mutex_destroy(&(thread->IDLE_mutex));
      pthread_mutex_destroy(&(thread->toThreadQueue.queueLock));
      pthread_mutex_destroy(&(thread->toProcessQueue.queueLock));
      
#ifdef TAGG_USE_LIBUV
      uv_close((uv_handle_t*) &thread->async_watcher, NULL);
      //uv_unref(&thread->async_watcher);
#else
      ev_async_stop(EV_DEFAULT_UC_ &thread->async_watcher);
      ev_unref(EV_DEFAULT_UC);
#endif

      thread->JSObject.Dispose();
      free(thread);
      return ThrowException(Exception::TypeError(String::New("create(): error in pthread_create()")));
    }

    return scope.Close(thread->JSObject);
}


void Init (Handle<Object> target) {
  
  freeJobsQueue= nuQueue();
  HandleScope scope;
  useLocker= v8::Locker::IsActive();
  id_symbol= Persistent<String>::New(String::NewSymbol("id"));
  
  target->Set(String::NewSymbol("create"), FunctionTemplate::New(Create)->GetFunction());
  target->Set(String::NewSymbol("createPool"), Script::Compile(String::New(kCreatePool_js))->Run()->ToObject());
  
  threadTemplate= Persistent<ObjectTemplate>::New(ObjectTemplate::New());
  threadTemplate->SetInternalFieldCount(1);
  threadTemplate->Set(id_symbol, Integer::New(0));
  threadTemplate->Set(String::NewSymbol("eval"), FunctionTemplate::New(Eval));
  threadTemplate->Set(String::NewSymbol("load"), FunctionTemplate::New(Load));
  threadTemplate->Set(String::NewSymbol("emit"), FunctionTemplate::New(processEmit));
  threadTemplate->Set(String::NewSymbol("destroy"), FunctionTemplate::New(Destroy));
  
}







NODE_MODULE(threads_a_gogo, Init)

/*
gcc -E -I /Users/jorge/JAVASCRIPT/binarios/include/node -o /o.c /Users/jorge/JAVASCRIPT/threads_a_gogo/src/threads_a_gogo.cc && mate /o.c
*/