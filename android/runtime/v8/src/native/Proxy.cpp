/*
 * Appcelerator Titanium Mobile
 * Copyright (c) 2011-2016 by Appcelerator, Inc. All Rights Reserved.
 * Licensed under the terms of the Apache Public License
 * Please see the LICENSE included with this distribution for details.
 */
#include <jni.h>
#include <string.h>
#include <v8.h>

#include "AndroidUtil.h"
#include "EventEmitter.h"
#include "JavaObject.h"
#include "JNIUtil.h"
#include "JSException.h"
#include "Proxy.h"
#include "ProxyFactory.h"
#include "TypeConverter.h"
#include "V8Util.h"

#define TAG "Proxy"
#define INDEX_NAME 0
#define INDEX_OLD_VALUE 1
#define INDEX_VALUE 2

using namespace v8;

namespace titanium {

Persistent<FunctionTemplate> Proxy::baseProxyTemplate;
Persistent<String> Proxy::javaClassSymbol;
Persistent<String> Proxy::constructorSymbol;
Persistent<String> Proxy::inheritSymbol;
Persistent<String> Proxy::propertiesSymbol;
Persistent<String> Proxy::lengthSymbol;
Persistent<String> Proxy::sourceUrlSymbol;

Proxy::Proxy(jobject javaProxy) :
	JavaObject(javaProxy)
{
}

void Proxy::bindProxy(Local<Object> exports, Local<Context> context)
{
	Isolate* isolate = context->GetIsolate();
	Local<String> javaClass = NEW_SYMBOL(isolate, "__javaClass__");
	javaClassSymbol.Reset(isolate, javaClass);
	constructorSymbol.Reset(isolate, NEW_SYMBOL(isolate, "constructor"));
	inheritSymbol.Reset(isolate, NEW_SYMBOL(isolate, "inherit"));
	propertiesSymbol.Reset(isolate, NEW_SYMBOL(isolate, "_properties"));
	lengthSymbol.Reset(isolate, NEW_SYMBOL(isolate, "length"));
	sourceUrlSymbol.Reset(isolate, NEW_SYMBOL(isolate, "sourceUrl"));

	Local<FunctionTemplate> proxyTemplate = FunctionTemplate::New(isolate);
	Local<String> proxySymbol = NEW_SYMBOL(isolate, "Proxy");
	proxyTemplate->InstanceTemplate()->SetInternalFieldCount(kInternalFieldCount);
	proxyTemplate->SetClassName(proxySymbol);
	proxyTemplate->Inherit(EventEmitter::constructorTemplate.Get(isolate));

	proxyTemplate->Set(javaClass, External::New(isolate, JNIUtil::krollProxyClass),
		static_cast<PropertyAttribute>(DontDelete | DontEnum));

	SetProtoMethod(isolate, proxyTemplate, "_hasListenersForEventType", hasListenersForEventType);
	SetProtoMethod(isolate, proxyTemplate, "onPropertiesChanged", proxyOnPropertiesChanged);
	SetProtoMethod(isolate, proxyTemplate, "_onEventFired", onEventFired);

	baseProxyTemplate.Reset(isolate, proxyTemplate);

	exports->Set(proxySymbol, proxyTemplate->GetFunction(context).ToLocalChecked());
}

static Local<Value> getPropertyForProxy(Isolate* isolate, Local<Name> property, Local<Object> proxy)
{
	// Call getProperty on the Proxy to get the property.
	// We define this method in JavaScript on the Proxy prototype.
	Local<Value> getProperty = proxy->Get(STRING_NEW(isolate, "getProperty"));
	if (!getProperty.IsEmpty() && getProperty->IsFunction()) {
		Local<Value> argv[1] = { property };
		MaybeLocal<Value> value = getProperty.As<Function>()->Call(isolate->GetCurrentContext(), proxy, 1, argv);
		if (value.IsEmpty()) {
			return Undefined(isolate);
		}
		return value.ToLocalChecked();
	}

	LOGE(TAG, "Unable to lookup Proxy.prototype.getProperty");
	return Undefined(isolate);
}

void Proxy::getProperty(Local<Name> property, const PropertyCallbackInfo<Value>& args)
{
	Isolate* isolate = args.GetIsolate();
	args.GetReturnValue().Set(getPropertyForProxy(isolate, property->ToString(isolate), args.Holder()));
}

void Proxy::getProperty(const FunctionCallbackInfo<Value>& args)
{
	Isolate* isolate = args.GetIsolate();
	// The name of the property can be passed either as
	// an argument or a data parameter.
	Local<String> name;
	if (args.Length() >= 1) {
		name = args[0]->ToString(isolate);
	} else if (args.Data()->IsString()) {
		name = args.Data().As<String>();
	} else {
		JSException::Error(isolate, "Requires property name.");
		return;
	}

	args.GetReturnValue().Set(getPropertyForProxy(isolate, name, args.Holder()));
}

static void setPropertyOnProxy(Isolate* isolate, Local<Name> property, Local<Value> value, Local<Object> proxy)
{
	// Call Proxy.prototype.setProperty.
	Local<Value> setProperty = proxy->Get(STRING_NEW(isolate, "setProperty"));
	if (!setProperty.IsEmpty() && setProperty->IsFunction()) {
		Local<Value> argv[2] = { property, value };
		setProperty.As<Function>()->Call(isolate->GetCurrentContext(), proxy, 2, argv);
		return;
	}

	LOGE(TAG, "Unable to lookup Proxy.prototype.setProperty");
}

void Proxy::setProperty(Local<Name> property, Local<Value> value, const PropertyCallbackInfo<void>& info)
{
	Isolate* isolate = info.GetIsolate();
	setPropertyOnProxy(isolate, property->ToString(isolate), value, info.This());
}

static void onPropertyChangedForProxy(Isolate* isolate, Local<String> property, Local<Value> value, Local<Object> proxyObject)
{
	Proxy* proxy = NativeObject::Unwrap<Proxy>(proxyObject);

	JNIEnv* env = JNIScope::getEnv();
	if (!env) {
		LOG_JNIENV_GET_ERROR(TAG);
		return;
	}

	jstring javaProperty = TypeConverter::jsStringToJavaString(env, property);
	bool javaValueIsNew;
	jobject javaValue = TypeConverter::jsValueToJavaObject(isolate, env, value, &javaValueIsNew);

	jobject javaProxy = proxy->getJavaObject();
	env->CallVoidMethod(javaProxy,
		JNIUtil::krollProxyOnPropertyChangedMethod,
		javaProperty,
		javaValue);

	if (!JavaObject::useGlobalRefs) {
		env->DeleteLocalRef(javaProxy);
	}

	env->DeleteLocalRef(javaProperty);
	if (javaValueIsNew) {
		env->DeleteLocalRef(javaValue);
	}

	// Store new property value on JS internal map.
	setPropertyOnProxy(isolate, property, value, proxyObject);
}

void Proxy::onPropertyChanged(Local<Name> property, Local<Value> value, const v8::PropertyCallbackInfo<void>& info)
{
	Isolate* isolate = info.GetIsolate();
	onPropertyChangedForProxy(isolate, property->ToString(isolate), value, info.Holder());
}

void Proxy::onPropertyChanged(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	Isolate* isolate = args.GetIsolate();
	if (args.Length() < 1) {
		JSException::Error(isolate, "Requires property name as first parameters.");
		return;
	}

	Local<String> name = args.Data()->ToString(isolate);
	Local<Value> value = args[0];
	onPropertyChangedForProxy(isolate, name, value, args.Holder());
}

void Proxy::getIndexedProperty(uint32_t index, const PropertyCallbackInfo<Value>& info)
{
	Isolate* isolate = info.GetIsolate();
	JNIEnv* env = JNIScope::getEnv();
	if (!env) {
		JSException::GetJNIEnvironmentError(isolate);
		return;
	}

	Proxy* proxy = NativeObject::Unwrap<Proxy>(info.Holder());
	jobject javaProxy = proxy->getJavaObject();
	jobject value = env->CallObjectMethod(javaProxy,
		JNIUtil::krollProxyGetIndexedPropertyMethod,
		index);

	if (!JavaObject::useGlobalRefs) {
		env->DeleteLocalRef(javaProxy);
	}

	Local<Value> result = TypeConverter::javaObjectToJsValue(isolate, env, value);
	env->DeleteLocalRef(value);

	info.GetReturnValue().Set(result);
}

void Proxy::setIndexedProperty(uint32_t index, Local<Value> value, const PropertyCallbackInfo<Value>& info)
{
	Isolate* isolate = info.GetIsolate();
	JNIEnv* env = JNIScope::getEnv();
	if (!env) {
		LOG_JNIENV_GET_ERROR(TAG);
		// Returns undefined by default
		return;
	}

	Proxy* proxy = NativeObject::Unwrap<Proxy>(info.Holder());

	bool javaValueIsNew;
	jobject javaValue = TypeConverter::jsValueToJavaObject(isolate, env, value, &javaValueIsNew);
	jobject javaProxy = proxy->getJavaObject();
	env->CallVoidMethod(javaProxy,
		JNIUtil::krollProxySetIndexedPropertyMethod,
		index,
		javaValue);

	if (!JavaObject::useGlobalRefs) {
		env->DeleteLocalRef(javaProxy);
	}
	if (javaValueIsNew) {
		env->DeleteLocalRef(javaValue);
	}

	info.GetReturnValue().Set(value);
}

void Proxy::hasListenersForEventType(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	Isolate* isolate = args.GetIsolate();
	JNIEnv* env = JNIScope::getEnv();
	if (!env) {
		JSException::GetJNIEnvironmentError(isolate);
		return;
	}

	Proxy* proxy = NativeObject::Unwrap<Proxy>(args.Holder());

	Local<String> eventType = args[0]->ToString(isolate);
	Local<Boolean> hasListeners = args[1]->ToBoolean(isolate);

	jobject javaProxy = proxy->getJavaObject();
	jobject krollObject = env->GetObjectField(javaProxy, JNIUtil::krollProxyKrollObjectField);
	jstring javaEventType = TypeConverter::jsStringToJavaString(env, eventType);

	if (!JavaObject::useGlobalRefs) {
		env->DeleteLocalRef(javaProxy);
	}

	env->CallVoidMethod(krollObject,
		JNIUtil::krollObjectSetHasListenersForEventTypeMethod,
		javaEventType,
		TypeConverter::jsBooleanToJavaBoolean(hasListeners));

	env->DeleteLocalRef(krollObject);
	env->DeleteLocalRef(javaEventType);
}

void Proxy::onEventFired(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	Isolate* isolate = args.GetIsolate();
	JNIEnv* env = JNIScope::getEnv();
	if (!env) {
		JSException::GetJNIEnvironmentError(isolate);
		return;
	}

	Proxy* proxy = NativeObject::Unwrap<Proxy>(args.Holder());

	Local<String> eventType = args[0]->ToString(isolate);
	Local<Value> eventData = args[1];

	jobject javaProxy = proxy->getJavaObject();
	jobject krollObject = env->GetObjectField(javaProxy, JNIUtil::krollProxyKrollObjectField);

	jstring javaEventType = TypeConverter::jsStringToJavaString(env, eventType);
	jobject javaEventData = TypeConverter::jsValueToJavaObject(isolate, env, eventData);


	if (!JavaObject::useGlobalRefs) {
		env->DeleteLocalRef(javaProxy);
	}

	env->CallVoidMethod(krollObject,
		JNIUtil::krollObjectOnEventFiredMethod,
		javaEventType,
		javaEventData);

	env->DeleteLocalRef(krollObject);
	env->DeleteLocalRef(javaEventType);
	env->DeleteLocalRef(javaEventData);
}

Local<FunctionTemplate> Proxy::inheritProxyTemplate(Isolate* isolate,
	Local<FunctionTemplate> superTemplate, jclass javaClass,
	Local<String> className, Local<Function> callback)
{
	EscapableHandleScope scope(isolate);

	Local<Value> wrappedClass = External::New(isolate, javaClass);
	Local<FunctionTemplate> inheritedTemplate = FunctionTemplate::New(isolate, proxyConstructor, callback);

	inheritedTemplate->Set(javaClassSymbol.Get(isolate), wrappedClass, static_cast<PropertyAttribute>(DontDelete | DontEnum));

	inheritedTemplate->InstanceTemplate()->SetInternalFieldCount(kInternalFieldCount);
	// every instance gets a special "_properties" object for us to use inetrnally for get/setProperty
	inheritedTemplate->InstanceTemplate()->Set(propertiesSymbol.Get(isolate), Object::New(isolate), static_cast<PropertyAttribute>(DontEnum));
	inheritedTemplate->SetClassName(className);
	inheritedTemplate->Inherit(superTemplate);

	return scope.Escape(inheritedTemplate);
}


void Proxy::proxyConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	LOGD(TAG, "proxy constructor callback!");
	Isolate* isolate = args.GetIsolate();
	EscapableHandleScope scope(isolate);

	JNIEnv *env = JNIScope::getEnv();
	Local<Object> jsProxy = args.This();

	// First things first, we need to wrap the object in case future calls need to unwrap proxy!
	Proxy* proxy = new Proxy(NULL);
	proxy->wrap(isolate, jsProxy);

	// Now we hook up a java Object from the JVM...
	jobject javaProxy = ProxyFactory::unwrapJavaProxy(args); // do we already have one that got passed in?
	bool deleteRef = false;
	if (!javaProxy) {
		// No passed in java object, so let's create an instance
		// Look up java class from prototype...
		Local<Object> prototype = jsProxy->GetPrototype()->ToObject(isolate);
		Local<Function> constructor = prototype->Get(constructorSymbol.Get(isolate)).As<Function>();
		Local<External> wrap = constructor->Get(javaClassSymbol.Get(isolate)).As<External>();
		jclass javaClass = static_cast<jclass>(wrap->Value());

		// Now we create an instance of the class and hook it up
		JNIUtil::logClassName("Creating java proxy for class %s", javaClass);
		javaProxy = ProxyFactory::createJavaProxy(javaClass, jsProxy, args);
		deleteRef = true;
	}
	proxy->attach(javaProxy);

	int length = args.Length();

	if (length > 0 && args[0]->IsObject()) {
		bool extend = true;
		Local<Object> createProperties = args[0].As<Object>();
		Local<String> constructorName = createProperties->GetConstructorName();
		if (strcmp(*titanium::Utf8Value(constructorName), "Arguments") == 0) {
			extend = false;
			int32_t argsLength = createProperties->Get(STRING_NEW(isolate, "length"))->Int32Value();
			if (argsLength > 1) {
				Local<Value> properties = createProperties->Get(1);
				if (properties->IsObject()) {
					extend = true;
					createProperties = properties.As<Object>();
				}
			}
		}

		if (extend) {
			Local<Array> names = createProperties->GetOwnPropertyNames();
			int length = names->Length();
			Local<Object> properties = jsProxy->Get(propertiesSymbol.Get(isolate))->ToObject(isolate);

			for (int i = 0; i < length; ++i) {
				Local<Value> name = names->Get(i);
				Local<Value> value = createProperties->Get(name);
				bool isProperty = true;
				if (name->IsString()) {
					Local<String> nameString = name.As<String>();
					if (!jsProxy->HasRealNamedCallbackProperty(nameString)
						&& !jsProxy->HasRealNamedProperty(nameString)) {
						jsProxy->Set(name, value);
						isProperty = false;
					}
				}
				if (isProperty) {
					properties->Set(name, value);
				}
			}
		}
	}


	if (!args.Data().IsEmpty() && args.Data()->IsFunction()) {
		Local<Function> proxyFn = args.Data().As<Function>();
		Local<Value> *fnArgs = new Local<Value>[length];
		for (int i = 0; i < length; ++i) {
			fnArgs[i] = args[i];
		}
		proxyFn->Call(isolate->GetCurrentContext(), jsProxy, length, fnArgs);
	}

	if (deleteRef) {
		JNIEnv *env = JNIScope::getEnv();
		if (env) {
			env->DeleteLocalRef(javaProxy);
		}
	}

	args.GetReturnValue().Set(scope.Escape(jsProxy));
}

void Proxy::proxyOnPropertiesChanged(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	Isolate* isolate = args.GetIsolate();
	HandleScope scope(isolate);
	Local<Object> jsProxy = args.Holder();

	if (args.Length() < 1 || !(args[0]->IsArray())) {
		JSException::Error(isolate, "Proxy.propertiesChanged requires a list of lists of property name, the old value, and the new value");
		return;
	}

	JNIEnv *env = JNIScope::getEnv();
	if (!env) {
		JSException::GetJNIEnvironmentError(isolate);
		return;
	}

	Proxy *proxy = unwrap(jsProxy);
	if (!proxy) {
		JSException::Error(isolate, "Failed to unwrap Proxy instance");
		return;
	}

	Local<Array> changes = args[0].As<Array>();
	uint32_t length = changes->Length();
	jobjectArray jChanges = env->NewObjectArray(length, JNIUtil::objectClass, NULL);

	for (uint32_t i = 0; i < length; ++i) {
		Local<Array> change = changes->Get(i).As<Array>();
		Local<String> name = change->Get(INDEX_NAME)->ToString(isolate);
		Local<Value> oldValue = change->Get(INDEX_OLD_VALUE);
		Local<Value> value = change->Get(INDEX_VALUE);

		jobjectArray jChange = env->NewObjectArray(3, JNIUtil::objectClass, NULL);

		jstring jName = TypeConverter::jsStringToJavaString(env, name);
		env->SetObjectArrayElement(jChange, INDEX_NAME, jName);
		env->DeleteLocalRef(jName);

		bool isNew;
		jobject jOldValue = TypeConverter::jsValueToJavaObject(isolate, env, oldValue, &isNew);
		env->SetObjectArrayElement(jChange, INDEX_OLD_VALUE, jOldValue);
		if (isNew) {
			env->DeleteLocalRef(jOldValue);
		}

		jobject jValue = TypeConverter::jsValueToJavaObject(isolate, env, value, &isNew);
		env->SetObjectArrayElement(jChange, INDEX_VALUE, jValue);
		if (isNew) {
			env->DeleteLocalRef(jValue);
		}

		env->SetObjectArrayElement(jChanges, i, jChange);
		env->DeleteLocalRef(jChange);
	}

	jobject javaProxy = proxy->getJavaObject();
	env->CallVoidMethod(javaProxy, JNIUtil::krollProxyOnPropertiesChangedMethod, jChanges);
	env->DeleteLocalRef(jChanges);

	if (!JavaObject::useGlobalRefs) {
		env->DeleteLocalRef(javaProxy);
	}

	return;
}

void Proxy::dispose()
{
	baseProxyTemplate.Reset();
	javaClassSymbol.Reset();
	constructorSymbol.Reset();
	inheritSymbol.Reset();
	propertiesSymbol.Reset();
	lengthSymbol.Reset();
	sourceUrlSymbol.Reset();
}

} // namespace titanium
