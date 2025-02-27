/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Sample Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include <algorithm>
#include "extension.h"
#include <dt_send.h>
#include <unordered_map>
#include <vector>
#include <mathlib/vector.h>
#include <iserverentity.h>
#include <iservernetworkable.h>
#include <server_class.h>
#include <ehandle.h>
#include <predictioncopy.h>
#include <utility>
#include <CDetour/detours.h>
#include <memory>
#include "packed_entity.h"
#include <iclient.h>
#include <igameevents.h>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <ISDKTools.h>

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

static Sample g_Sample;		/**< Global singleton for extension's main interface */

SMEXT_LINK(&g_Sample);

static IGameConfig *gameconf{nullptr};
static ISDKHooks *g_pSDKHooks{nullptr};
ISDKTools *g_pSDKTools = nullptr;

IServer *server = nullptr;
CBaseEntityList *g_pEntityList = nullptr;

static void *CGameClient_GetSendFrame_ptr{nullptr};

static int CBaseClient_UpdateSendState_idx{-1};
static int CBaseClient_SendSnapshot_idx{-1};

template <typename T>
int vfunc_index(T func)
{
	SourceHook::MemFuncInfo info{};
	SourceHook::GetFuncInfo<T>(func, info);
	return info.vtblindex;
}

template <typename R, typename T, typename ...Args>
R call_mfunc(T *pThisPtr, void *offset, Args ...args)
{
	class VEmptyClass {};
	
	void **this_ptr = *reinterpret_cast<void ***>(&pThisPtr);
	
	union
	{
		R (VEmptyClass::*mfpnew)(Args...);
#ifndef PLATFORM_POSIX
		void *addr;
	} u;
	u.addr = offset;
#else
		struct  
		{
			void *addr;
			intptr_t adjustor;
		} s;
	} u;
	u.s.addr = offset;
	u.s.adjustor = 0;
#endif
	
	return (R)(reinterpret_cast<VEmptyClass *>(this_ptr)->*u.mfpnew)(args...);
}

template <typename R, typename T, typename ...Args>
R call_vfunc(T *pThisPtr, size_t offset, Args ...args)
{
	void **vtable = *reinterpret_cast<void ***>(pThisPtr);
	void *vfunc = vtable[offset];
	
	return call_mfunc<R, T, Args...>(pThisPtr, vfunc, args...);
}

class CFrameSnapshot;
class CClientFrame;

class CBaseClient : public IGameEventListener2, public IClient, public IClientMessageHandler
{
};

class CGameClient : public CBaseClient
{
public:
	inline void SendSnapshot(CClientFrame *pFrame)
	{ call_vfunc<void>(this, CBaseClient_SendSnapshot_idx, pFrame); }

	inline void UpdateSendState()
	{ call_vfunc<void>(this, CBaseClient_UpdateSendState_idx); }

	inline CClientFrame *GetSendFrame()
	{ return call_mfunc<CClientFrame *>(this, CGameClient_GetSendFrame_ptr); }
};

class CBaseEntity : public IServerEntity
{
};

using prop_types = proxysend::prop_types;

static void global_send_proxy(const SendProp *pProp, const void *pStructBase, const void *pData, DVariant *pOut, int iElement, int objectID);

static const CStandardSendProxies *std_proxies;

#if SOURCE_ENGINE == SE_TF2
static const SendProp *m_nPlayerCond{nullptr};
static const SendProp *_condition_bits{nullptr};
static const SendProp *m_nPlayerCondEx{nullptr};
static const SendProp *m_nPlayerCondEx2{nullptr};
static const SendProp *m_nPlayerCondEx3{nullptr};
static const SendProp *m_nPlayerCondEx4{nullptr};

static bool is_prop_cond(const SendProp *pProp)
{
	return (pProp == m_nPlayerCond ||
			pProp == _condition_bits ||
			pProp == m_nPlayerCondEx ||
			pProp == m_nPlayerCondEx2 ||
			pProp == m_nPlayerCondEx3 ||
			pProp == m_nPlayerCondEx4);
}
#endif

struct proxyrestore_t final
{
	inline proxyrestore_t(proxyrestore_t &&other) noexcept
	{ operator=(std::move(other)); }

	proxyrestore_t(SendProp *pProp_, prop_types type_) noexcept
		: pProp{pProp_}, pRealProxy{pProp->GetProxyFn()}, type{type_}
	{
	#ifdef _DEBUG
		printf("set %s proxy func\n", pProp->GetName());
	#endif
		pProp->SetProxyFn(global_send_proxy);
	}

	~proxyrestore_t() noexcept {
		if(pProp && pRealProxy) {
		#ifdef _DEBUG
			printf("reset %s proxy func\n", pProp->GetName());
		#endif
			pProp->SetProxyFn(pRealProxy);
		}
	}

	proxyrestore_t &operator=(proxyrestore_t &&other) noexcept
	{
		pProp = other.pProp;
		other.pProp = nullptr;
		pRealProxy = other.pRealProxy;
		other.pRealProxy = nullptr;
		type = other.type;
		return *this;
	}

	SendProp *pProp{nullptr};
	SendVarProxyFn pRealProxy{nullptr};
	std::size_t ref{0};
	prop_types type{prop_types::unknown};

private:
	proxyrestore_t(const proxyrestore_t &) = delete;
	proxyrestore_t &operator=(const proxyrestore_t &) = delete;
	proxyrestore_t() = delete;
};

using restores_t = std::unordered_map<SendProp *, std::unique_ptr<proxyrestore_t>>;
static restores_t restores;

static prop_types guess_prop_type(const SendProp *pProp, const SendTable *pTable) noexcept
{
#if defined _DEBUG
	printf("%s type is ", pProp->GetName());
#endif

	SendVarProxyFn pRealProxy{pProp->GetProxyFn()};
	if(pRealProxy == global_send_proxy) {
		restores_t::const_iterator it_restore{restores.find(const_cast<SendProp *>(pProp))};
		if(it_restore == restores.cend()) {
		#if defined _DEBUG
			printf("invalid (global send proxy)\n");
		#endif
			return prop_types::unknown;
		}

	#if defined _DEBUG
		printf("from restore (global send proxy)\n");
	#endif
		return it_restore->second->type;
	}

#if SOURCE_ENGINE == SE_TF2
	if(is_prop_cond(pProp)) {
	#if defined _DEBUG
		printf("unsigned int (is cond)\n");
	#endif
		return prop_types::unsigned_int;
	}
#endif

	switch(pProp->GetType()) {
		case DPT_Int: {
			if(pProp->GetFlags() & SPROP_UNSIGNED) {
				if(pRealProxy == std_proxies->m_UInt8ToInt32) {
					if(pProp->m_nBits == 1) {
					#if defined _DEBUG
						printf("bool (bits == 1)\n");
					#endif
						return prop_types::bool_;
					}

				#if defined _DEBUG
					printf("unsigned char (std proxy)\n");
				#endif
					return prop_types::unsigned_char;
				} else if(pRealProxy == std_proxies->m_UInt16ToInt32) {
				#if defined _DEBUG
					printf("unsigned short (std proxy)\n");
				#endif
					return prop_types::unsigned_short;
				} else if(pRealProxy == std_proxies->m_UInt32ToInt32) {
					if(pTable && strcmp(pTable->GetName(), "DT_BaseEntity") == 0 && strcmp(pProp->GetName(), "m_clrRender") == 0) {
					#if defined _DEBUG
						printf("color32 (hardcode)\n");
					#endif
						return prop_types::color32_;
					}

				#if defined _DEBUG
					printf("unsigned int (std proxy)\n");
				#endif
					return prop_types::unsigned_int;
				} else {
					{
						if(pProp->m_nBits == 32) {
							struct dummy_t {
								unsigned int val{256};
							} dummy;

							DVariant out{};
							pRealProxy(pProp, static_cast<const void *>(&dummy), static_cast<const void *>(&dummy.val), &out, 0, -1);
							if(out.m_Int == 65536) {
							#if defined _DEBUG
								printf("color32 (proxy)\n");
							#endif
								return prop_types::color32_;
							}
						}
					}

					{
						if(pProp->m_nBits == NUM_NETWORKED_EHANDLE_BITS) {
							struct dummy_t {
								EHANDLE val{};
							} dummy;

							DVariant out{};
							pRealProxy(pProp, static_cast<const void *>(&dummy), static_cast<const void *>(&dummy.val), &out, 0, -1);
							if(out.m_Int == INVALID_NETWORKED_EHANDLE_VALUE) {
							#if defined _DEBUG
								printf("ehandle (proxy)\n");
							#endif
								return prop_types::ehandle;
							}
						}
					}

				#if defined _DEBUG
					printf("unsigned int (flag)\n");
				#endif
					return prop_types::unsigned_int;
				}
			} else {
				if(pRealProxy == std_proxies->m_Int8ToInt32) {
				#if defined _DEBUG
					printf("char (std proxy)\n");
				#endif
					return prop_types::char_;
				} else if(pRealProxy == std_proxies->m_Int16ToInt32) {
				#if defined _DEBUG
					printf("short (std proxy)\n");
				#endif
					return prop_types::short_;
				} else if(pRealProxy == std_proxies->m_Int32ToInt32) {
				#if defined _DEBUG
					printf("int (std proxy)\n");
				#endif
					return prop_types::int_;
				} else {
					{
						struct dummy_t {
							short val{SHRT_MAX-1};
						} dummy;

						DVariant out{};
						pRealProxy(pProp, static_cast<const void *>(&dummy), static_cast<const void *>(&dummy.val), &out, 0, -1);
						if(out.m_Int == dummy.val+1) {
						#if defined _DEBUG
							printf("short (proxy)\n");
						#endif
							return prop_types::short_;
						}
					}

				#if defined _DEBUG
					printf("int (type)\n");
				#endif
					return prop_types::int_;
				}
			}
		}
		case DPT_Float:
		return prop_types::float_;
		case DPT_Vector: {
			if(pProp->m_fLowValue == 0.0f && pProp->m_fHighValue == 360.0f) {
				return prop_types::qangle;
			} else {
				return prop_types::vector;
			}
		}
		case DPT_VectorXY:
		return prop_types::vector;
		case DPT_String: {
			return prop_types::cstring;
		}
		case DPT_Array:
		return prop_types::unknown;
		case DPT_DataTable:
		return prop_types::unknown;
	}

	return prop_types::unknown;
}

proxysend::prop_types Sample::guess_prop_type(const SendProp *prop, const SendTable *table) const noexcept
{
	return ::guess_prop_type(prop, table);
}

template <typename T>
class thread_var_base
{
protected:
	using ptr_ret_t = std::conditional_t<std::is_pointer<T>::value, T, T *>;

public:
	inline void reset(std::nullptr_t) noexcept
	{ reset_ptr(nullptr); }

	thread_var_base &operator=(std::nullptr_t) noexcept
	{
		reset_ptr(nullptr);
		return *this;
	}

	inline thread_var_base(std::nullptr_t) noexcept
		: thread_var_base{}
	{
	}

	inline thread_var_base() noexcept
	{
	}

	inline bool operator!() const noexcept;

	inline ptr_ret_t operator->() noexcept
	{ return get_ptr(); }

	inline ~thread_var_base() noexcept
	{
		if(allocated_) {
			unallocate();
		}
	}

protected:
	static constexpr const pthread_key_t invalid_key{PTHREAD_KEYS_MAX+1};

	pthread_key_t key{invalid_key};
	bool allocated_{false};

	static void dtor(void *ptr) noexcept
	{ delete reinterpret_cast<T *>(ptr); }

	T *get_ptr_raw() const noexcept
	{
		if(!allocated_) {
			return nullptr;
		}
		return reinterpret_cast<T *>(pthread_getspecific(key));
	}

	ptr_ret_t get_ptr() const noexcept
	{
		T *ptr{get_ptr_raw()};
		if(!ptr) {
			return nullptr;
		}
	#if 0
		if constexpr(std::is_pointer_v<T>) {
			return *ptr;
		} else
	#endif
		{
			return ptr;
		}
	}

	T *get_or_allocate_ptr() noexcept
	{
		if(!allocated_ && !allocate()) {
			return nullptr;
		}
		T *ptr{reinterpret_cast<T *>(pthread_getspecific(key))};
		if(!ptr) {
			ptr = new T{};
			pthread_setspecific(key, ptr);
		}
		return ptr;
	}

	bool allocate() noexcept
	{
		if(!__sync_bool_compare_and_swap(&allocated_, 0, 1)) {
			return true;
		}
		if(pthread_key_create(&key, dtor) != 0) {
			return false;
		}
		pthread_setspecific(key, new T{});
		return true;
	}

	bool unallocate() noexcept
	{
		if(!__sync_bool_compare_and_swap(&allocated_, 1, 0)) {
			return true;
		}
		T *old_ptr{reinterpret_cast<T *>(pthread_getspecific(key))};
		if(old_ptr) {
			delete old_ptr;
		}
		return (pthread_key_delete(key) == 0);
	}

	void reset_ptr(T *ptr) noexcept
	{
		T *old_ptr{get_ptr_raw()};
		if(old_ptr) {
			delete old_ptr;
		}
		if(!allocated_ && !allocate()) {
			return;
		}
		pthread_setspecific(key, ptr);
	}

private:
	thread_var_base(const thread_var_base &) = delete;
	thread_var_base &operator=(const thread_var_base &) = delete;
	thread_var_base(thread_var_base &&) = delete;
	thread_var_base &operator=(thread_var_base &&) = delete;
};

template <>
inline bool thread_var_base<bool>::operator!() const noexcept
{
	const bool *ptr{get_ptr_raw()};
	return (!ptr || !*ptr);
}

template <typename T>
inline bool thread_var_base<T>::operator!() const noexcept
{ return get_ptr() == nullptr; }

template <typename T>
class thread_var final : public thread_var_base<T>
{
public:
	using thread_var_base<T>::thread_var_base;
	using thread_var_base<T>::reset;

	template <typename ...Args>
	T &reset(Args &&...args) noexcept
	{
		T *ptr{this->get_or_allocate_ptr()};
		ptr->~T();
		new (ptr) T{std::forward<Args>(args)...};
		return *ptr;
	}

	inline thread_var() noexcept
		: thread_var_base<T>{}
	{
	}

	inline thread_var(const T &val) noexcept
	{ *this->get_or_allocate_ptr() = val; }

	inline thread_var(T &&val) noexcept
	{ *this->get_or_allocate_ptr() = std::move(val); }

	template <typename ...Args>
	inline thread_var(Args &&...args) noexcept
	{
		T *ptr{this->get_or_allocate_ptr()};
		ptr->~T();
		new (ptr) T{std::forward<Args>(args)...};
	}

	thread_var &operator=(std::nullptr_t) noexcept
	{
		this->reset_ptr(nullptr);
		return *this;
	}

	thread_var &operator=(const T &val) noexcept
	{
		*this->get_or_allocate_ptr() = val;
		return *this;
	}

	thread_var &operator=(T &&val) noexcept
	{
		*this->get_or_allocate_ptr() = std::move(val);
		return *this;
	}

	inline T &operator*() noexcept
	{ return *this->get_ptr_raw(); }

	inline T &get() noexcept
	{ return *this->get_ptr_raw(); }

	inline operator T &() noexcept
	{ return *this->get_ptr_raw(); }

	inline explicit operator bool() const noexcept
	{ return this->get_ptr() != nullptr; }
};

template <>
class thread_var<bool> final : public thread_var_base<bool>
{
public:
	using thread_var_base<bool>::thread_var_base;
	using thread_var_base<bool>::reset;

	inline thread_var()
		: thread_var_base<bool>{}
	{
	}

	inline bool operator*() const noexcept
	{ return get(); }

	inline bool get() const noexcept
	{
		const bool *ptr{get_ptr_raw()};
		return (ptr && *ptr);
	}

	inline operator bool() const noexcept
	{ return get(); }

	bool reset(bool val) noexcept
	{
		set_value(val);
		return val;
	}

	thread_var &operator=(std::nullptr_t) noexcept
	{
		set_value(false);
		return *this;
	}

	inline thread_var(bool val) noexcept
	{ set_value(val); }

	thread_var &operator=(bool val) noexcept
	{
		set_value(val);
		return *this;
	}

private:
	void set_value(bool val) noexcept
	{
		*this->get_or_allocate_ptr() = val;
	}
};

struct packed_entity_data_t final
{
	packed_entity_data_t(packed_entity_data_t &&other) noexcept
	{ operator=(std::move(other)); }
	packed_entity_data_t &operator=(packed_entity_data_t &&other) noexcept {
		packedData = other.packedData;
		other.packedData = nullptr;
		writeBuf = other.writeBuf;
		other.writeBuf = nullptr;
		ref = other.ref;
		other.ref = INVALID_EHANDLE_INDEX;
		return *this;
	}

	char *packedData{nullptr};
	bf_write *writeBuf{nullptr};
	int ref{INVALID_EHANDLE_INDEX};

	bool allocated() const noexcept
	{ return (packedData && writeBuf); }

	bool written() const noexcept
	{ return allocated() && (writeBuf->GetNumBitsWritten() > 0); }

	packed_entity_data_t() noexcept = default;
	~packed_entity_data_t() noexcept {
		reset();
	}

	void reset() noexcept {
		if(writeBuf) {
			delete writeBuf;
			writeBuf = nullptr;
		}
		if(packedData) {
			free(packedData);
			packedData = nullptr;
		}
	}

	void allocate() noexcept {
		reset();

		packedData = static_cast<char *>(aligned_alloc(4, MAX_PACKEDENTITY_DATA));
		writeBuf = new bf_write{"SV_PackEntity->writeBuf", packedData, MAX_PACKEDENTITY_DATA};
	}

private:
	packed_entity_data_t(const packed_entity_data_t &) = delete;
	packed_entity_data_t &operator=(const packed_entity_data_t &) = delete;
};

struct pack_entity_params_t final
{
	std::vector<std::vector<packed_entity_data_t>> entity_data{};
	std::vector<int> slots{};
	std::vector<int> entities{};
	int snapshot_index{-1};

	pack_entity_params_t(std::vector<int> &&slots_, std::vector<int> &&entities_, int snapshot_index_) noexcept
		: slots{std::move(slots_)}, entities{std::move(entities_)}, snapshot_index{snapshot_index_}
	{
		entity_data.resize(slots.size());
	}
	~pack_entity_params_t() noexcept = default;

private:
	pack_entity_params_t(const pack_entity_params_t &) = delete;
	pack_entity_params_t &operator=(const pack_entity_params_t &) = delete;
	pack_entity_params_t(pack_entity_params_t &&) = delete;
	pack_entity_params_t &operator=(pack_entity_params_t &&) = delete;
};

static thread_var<bool> in_compute_packs;
static thread_var<bool> do_calc_delta;
static thread_var<bool> do_writedelta_entities;
static thread_var<int> writedeltaentities_client;
static thread_var<int> sendproxy_client_slot;

static std::unique_ptr<pack_entity_params_t> packentity_params;

static void Host_Error(const char *error, ...) noexcept
{
	va_list argptr;
	char string[1024];

	va_start(argptr, error);
	Q_vsnprintf(string, sizeof(string), error, argptr);
	va_end(argptr);

	Error("Host_Error: %s", string);
}

struct prop_reference_t
{
	prop_reference_t(SendProp *pProp, prop_types type) noexcept
	{
		restores_t::iterator it_restore{restores.find(pProp)};
		if(it_restore == restores.end()) {
			std::unique_ptr<proxyrestore_t> ptr{new proxyrestore_t{pProp, type}};
			it_restore = restores.emplace(std::pair<SendProp *, std::unique_ptr<proxyrestore_t>>{pProp, std::move(ptr)}).first;
		}
		restore = it_restore->second.get();
		++restore->ref;
	#ifdef _DEBUG
		printf("added ref %zu for %s %p\n", restore->ref, pProp->GetName(), pProp);
	#endif
	}

	virtual ~prop_reference_t() noexcept
	{
		if(restore) {
		#ifdef _DEBUG
			printf("removed ref %zu for %s %p\n", restore->ref-1u, restore->pProp->GetName(), restore->pProp);
		#endif
			if(--restore->ref == 0) {
				restores_t::iterator it_restore{restores.begin()};
				while(it_restore != restores.end()) {
					if(it_restore->second.get() == restore) {
						restores.erase(it_restore);
						break;
					}
					++it_restore;
				}
			}
		}
	}

	inline prop_reference_t(prop_reference_t &&other) noexcept
	{ operator=(std::move(other)); }

	prop_reference_t &operator=(prop_reference_t &&other) noexcept
	{
		restore = other.restore;
		other.restore = nullptr;
		return *this;
	}

	proxyrestore_t *restore{nullptr};

private:
	prop_reference_t(const prop_reference_t &) = delete;
	prop_reference_t &operator=(const prop_reference_t &) = delete;
	prop_reference_t() = delete;
};

struct callback_t;

struct opaque_ptr final
{
	inline opaque_ptr(opaque_ptr &&other) noexcept
	{ operator=(std::move(other)); }

	template <typename T>
	static void del_hlpr(void *ptr_) noexcept
	{ delete[] static_cast<T *>(ptr_); }

	opaque_ptr() = default;

	template <typename T, typename ...Args>
	void emplace(std::size_t num, Args &&...args) noexcept {
		if(del_func && ptr) {
			del_func(ptr);
		}
		ptr = static_cast<void *>(new T{std::forward<Args>(args)...});
		del_func = del_hlpr<T>;
	}

	void clear() noexcept {
		if(del_func && ptr) {
			del_func(ptr);
		}
		del_func = nullptr;
		ptr = nullptr;
	}

	template <typename T>
	T &get(std::size_t element) noexcept
	{ return static_cast<T *>(ptr)[element]; }
	template <typename T = void>
	T *get() noexcept
	{ return static_cast<T *>(ptr); }

	template <typename T>
	const T &get(std::size_t element) const noexcept
	{ return static_cast<const T *>(ptr)[element]; }
	template <typename T = void>
	const T *get() const noexcept
	{ return static_cast<const T *>(ptr); }

	~opaque_ptr() noexcept {
		if(del_func && ptr) {
			del_func(ptr);
		}
	}

	opaque_ptr &operator=(opaque_ptr &&other) noexcept
	{
		ptr = other.ptr;
		other.ptr = nullptr;
		del_func = other.del_func;
		other.del_func = nullptr;
		return *this;
	}

private:
	opaque_ptr(const opaque_ptr &) = delete;
	opaque_ptr &operator=(const opaque_ptr &) = delete;

	void *ptr{nullptr};
	void (*del_func)(void *) {nullptr};
};

struct callback_t final : prop_reference_t
{
	callback_t(int ref_, SendProp *pProp, std::string &&name_, int element_, prop_types type_, std::size_t offset_) noexcept
		: prop_reference_t{pProp, type_}, offset{offset_}, type{type_}, element{element_}, name{std::move(name_)}, prop{pProp}, ref{ref_}
	{
		if(type == prop_types::cstring || type == prop_types::tstring) {
			fwd = forwards->CreateForwardEx(nullptr, ET_Hook, 6, nullptr, Param_Cell, Param_String, Param_String, Param_Cell, Param_Cell, Param_Cell);
		} else if(type == prop_types::color32_) {
			fwd = forwards->CreateForwardEx(nullptr, ET_Hook, 8, nullptr, Param_Cell, Param_String, Param_CellByRef, Param_CellByRef, Param_CellByRef, Param_CellByRef, Param_Cell, Param_Cell);
		} else {
			ParamType value_param_type;

			switch(type) {
				case prop_types::int_:
				case prop_types::short_:
				case prop_types::char_:
				case prop_types::unsigned_int:
				case prop_types::unsigned_short:
				case prop_types::unsigned_char:
				case prop_types::bool_:
				case prop_types::ehandle: {
					value_param_type = Param_CellByRef;
				} break;
				case prop_types::float_: {
					value_param_type = Param_FloatByRef;
				} break;
				case prop_types::vector:
				case prop_types::qangle: {
					value_param_type = Param_Array;
				} break;
			}

			fwd = forwards->CreateForwardEx(nullptr, ET_Hook, 5, nullptr, Param_Cell, Param_String, value_param_type, Param_Cell, Param_Cell);
		}
	}

	inline bool has_any_per_client_func() const noexcept
	{ return !per_client_funcs.empty(); }

	void change_edict_state() noexcept
	{
		if(ref != INVALID_EHANDLE_INDEX) {
			CBaseEntity *pEntity{gamehelpers->ReferenceToEntity(ref)};
			edict_t *edict{pEntity->GetNetworkable()->GetEdict()};
			if(edict) {
				gamehelpers->SetEdictStateChanged(edict, offset);
			}
		}
	}

	void add_function(IPluginFunction *func, bool per_client) noexcept
	{
		fwd->RemoveFunction(func);
		fwd->AddFunction(func);

		if(per_client) {
			bool found{false};

			per_client_funcs_t::const_iterator it_func{per_client_funcs.cbegin()};
			while(it_func != per_client_funcs.cend()) {
				if(it_func->func == func) {
					found = true;
					break;
				}
				++it_func;
			}

			if(!found) {
				per_client_funcs.emplace_back(per_client_func_t{func});
			}
		}
	}

	void remove_function(IPluginFunction *func) noexcept
	{
		fwd->RemoveFunction(func);

		per_client_funcs_t::const_iterator it_func{per_client_funcs.cbegin()};
		while(it_func != per_client_funcs.cend()) {
			if(it_func->func == func) {
				per_client_funcs.erase(it_func);
				break;
			}
			++it_func;
		}
	}

	void remove_functions_of_plugin(IPlugin *plugin) noexcept
	{
		per_client_funcs_t::const_iterator it_func{per_client_funcs.cbegin()};
		while(it_func != per_client_funcs.cend()) {
			if((*it_func).func->GetParentContext() == plugin->GetBaseContext()) {
				it_func = per_client_funcs.erase(it_func);
				continue;
			}
			++it_func;
		}

		fwd->RemoveFunctionsOfPlugin(plugin);
	}

	~callback_t() noexcept override final {
		if(fwd) {
			forwards->ReleaseForward(fwd);
		}
	}

	static int get_current_client_slot() noexcept
	{
		if(!sendproxy_client_slot) {
			return -1;
		}

		return sendproxy_client_slot;
	}

	static int get_current_client_entity() noexcept
	{
		int slot{get_current_client_slot()};
		if(slot == -1) {
			return -1;
		}

		return slot+1;
	}

	bool fwd_call_ehandle(int client, const SendProp *pProp, const void *old_pData, opaque_ptr &new_pData, int objectID) const noexcept
	{
		fwd->PushCell(objectID);
		fwd->PushStringEx((char *)name.c_str(), name.size()+1, SM_PARAM_STRING_COPY|SM_PARAM_STRING_UTF8, 0);
		const EHANDLE &hndl{*reinterpret_cast<const EHANDLE *>(old_pData)};
		CBaseEntity *pEntity = hndl.Get();
		cell_t sp_value{pEntity ? gamehelpers->EntityToBCompatRef(pEntity) : -1};
		fwd->PushCellByRef(&sp_value);
		fwd->PushCell(element);
		fwd->PushCell(client);
		cell_t res{Pl_Continue};
		fwd->Execute(&res);
		if(res == Pl_Changed) {
			new_pData.emplace<EHANDLE>(1);
			EHANDLE &new_value{new_pData.get<EHANDLE>(0)};
			pEntity = gamehelpers->ReferenceToEntity(sp_value);
			if(pEntity) {
				new_value = pEntity->GetRefEHandle();
			}
			return true;
		}
		return false;
	}

	bool fwd_call_color32(int client, const SendProp *pProp, const void *old_pData, opaque_ptr &new_pData, int objectID) const noexcept
	{
		fwd->PushCell(objectID);
		fwd->PushStringEx((char *)name.c_str(), name.size()+1, SM_PARAM_STRING_COPY|SM_PARAM_STRING_UTF8, 0);
		const color32 &clr{*reinterpret_cast<const color32 *>(old_pData)};
		cell_t sp_r{static_cast<cell_t>(clr.r)};
		cell_t sp_g{static_cast<cell_t>(clr.g)};
		cell_t sp_b{static_cast<cell_t>(clr.b)};
		cell_t sp_a{static_cast<cell_t>(clr.a)};
		fwd->PushCellByRef(&sp_r);
		fwd->PushCellByRef(&sp_g);
		fwd->PushCellByRef(&sp_b);
		fwd->PushCellByRef(&sp_a);
		fwd->PushCell(element);
		fwd->PushCell(client);
		cell_t res{Pl_Continue};
		fwd->Execute(&res);
		if(res == Pl_Changed) {
			new_pData.emplace<color32>(1);
			color32 &new_value{new_pData.get<color32>(0)};
			new_value.r = static_cast<byte>(sp_r);
			new_value.g = static_cast<byte>(sp_g);
			new_value.b = static_cast<byte>(sp_b);
			new_value.a = static_cast<byte>(sp_a);
			return true;
		}
		return false;
	}

	template <typename T>
	bool fwd_call_int(int client, const SendProp *pProp, const void *old_pData, opaque_ptr &new_pData, int objectID) const noexcept
	{
		fwd->PushCell(objectID);
		fwd->PushStringEx((char *)name.c_str(), name.size()+1, SM_PARAM_STRING_COPY|SM_PARAM_STRING_UTF8, 0);
		cell_t sp_value{static_cast<cell_t>(*reinterpret_cast<const T *>(old_pData))};
		fwd->PushCellByRef(&sp_value);
		fwd->PushCell(element);
		fwd->PushCell(client);
		cell_t res{Pl_Continue};
		fwd->Execute(&res);
		if(res == Pl_Changed) {
			new_pData.emplace<T>(1);
			T &new_value{new_pData.get<T>(0)};
			new_value = static_cast<T>(sp_value);
			return true;
		}
		return false;
	}

	bool fwd_call_float(int client, const SendProp *pProp, const void *old_pData, opaque_ptr &new_pData, int objectID) const noexcept
	{
		fwd->PushCell(objectID);
		fwd->PushStringEx((char *)name.c_str(), name.size()+1, SM_PARAM_STRING_COPY|SM_PARAM_STRING_UTF8, 0);
		float sp_value{static_cast<float>(*reinterpret_cast<const float *>(old_pData))};
		fwd->PushFloatByRef(&sp_value);
		fwd->PushCell(element);
		fwd->PushCell(client);
		cell_t res{Pl_Continue};
		fwd->Execute(&res);
		if(res == Pl_Changed) {
			new_pData.emplace<float>(1);
			float &new_value{new_pData.get<float>(0)};
			new_value = sp_value;
			return true;
		}
		return false;
	}

	template <typename T>
	bool fwd_call_vec(int client, const SendProp *pProp, const void *old_pData, opaque_ptr &new_pData, int objectID) const noexcept
	{
		fwd->PushCell(objectID);
		fwd->PushStringEx((char *)name.c_str(), name.size()+1, SM_PARAM_STRING_COPY|SM_PARAM_STRING_UTF8, 0);
		const T &vec{*reinterpret_cast<const T *>(old_pData)};
		cell_t sp_value[3]{
			sp_ftoc(vec[0]),
			sp_ftoc(vec[1]),
			sp_ftoc(vec[2])
		};
		fwd->PushArray(sp_value, 3, SM_PARAM_COPYBACK);
		fwd->PushCell(element);
		fwd->PushCell(client);
		cell_t res{Pl_Continue};
		fwd->Execute(&res);
		if(res == Pl_Changed) {
			new_pData.emplace<T>(1);
			T &new_value{new_pData.get<T>(0)};
			new_value.x = sp_ctof(sp_value[0]);
			new_value.y = sp_ctof(sp_value[1]);
			new_value.z = sp_ctof(sp_value[2]);
			return true;
		}
		return false;
	}

	bool fwd_call_str(int client, const SendProp *pProp, const void *old_pData, opaque_ptr &new_pData, int objectID) const noexcept
	{
		fwd->PushCell(objectID);
		fwd->PushStringEx((char *)name.c_str(), name.size()+1, SM_PARAM_STRING_COPY|SM_PARAM_STRING_UTF8, 0);
		static char sp_value[4096];
		strcpy(sp_value, reinterpret_cast<const char *>(old_pData));
		fwd->PushStringEx(sp_value, sizeof(sp_value), SM_PARAM_STRING_UTF8|SM_PARAM_STRING_COPY, SM_PARAM_COPYBACK);
		fwd->PushCell(sizeof(sp_value));
		fwd->PushCell(element);
		fwd->PushCell(client);
		cell_t res{Pl_Continue};
		fwd->Execute(&res);
		if(res == Pl_Changed) {
			new_pData.emplace<char>(strlen(sp_value)+1);
			char *new_value{new_pData.get<char>()};
			strcpy(new_value, new_value);
			return true;
		}
		return false;
	}

	bool fwd_call_tstr(int client, const SendProp *pProp, const void *old_pData, opaque_ptr &new_pData, int objectID) const noexcept
	{
		fwd->PushCell(objectID);
		fwd->PushStringEx((char *)name.c_str(), name.size()+1, SM_PARAM_STRING_COPY|SM_PARAM_STRING_UTF8, 0);
		static char sp_value[4096];
		strcpy(sp_value, STRING(*reinterpret_cast<const string_t *>(old_pData)));
		fwd->PushStringEx(sp_value, sizeof(sp_value), SM_PARAM_STRING_UTF8|SM_PARAM_STRING_COPY, SM_PARAM_COPYBACK);
		fwd->PushCell(sizeof(sp_value));
		fwd->PushCell(element);
		fwd->PushCell(client);
		cell_t res{Pl_Continue};
		fwd->Execute(&res);
		if(res == Pl_Changed) {
			new_pData.emplace<string_t>(1);
			string_t &new_value{new_pData.get<string_t>(0)};
			new_value = MAKE_STRING(sp_value);
			return true;
		}
		return false;
	}

	bool can_call_fwd(int client) const noexcept
	{
		if(!fwd || (has_any_per_client_func() && client == -1)) {
			return false;
		}
		return true;
	}

	bool fwd_call(int client, const SendProp *pProp, const void *old_pData, opaque_ptr &new_pData, int objectID) const noexcept
	{
		switch(type) {
			case prop_types::int_:
			return fwd_call_int<int>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::bool_:
			return fwd_call_int<bool>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::short_:
			return fwd_call_int<short>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::char_:
			return fwd_call_int<char>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::unsigned_int:
			return fwd_call_int<unsigned int>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::unsigned_short:
			return fwd_call_int<unsigned short>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::unsigned_char:
			return fwd_call_int<unsigned char>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::float_:
			return fwd_call_float(client, pProp, old_pData, new_pData, objectID);
			case prop_types::vector:
			return fwd_call_vec<Vector>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::qangle:
			return fwd_call_vec<QAngle>(client, pProp, old_pData, new_pData, objectID);
			case prop_types::color32_:
			return fwd_call_color32(client, pProp, old_pData, new_pData, objectID);
			case prop_types::ehandle:
			return fwd_call_ehandle(client, pProp, old_pData, new_pData, objectID);
			case prop_types::cstring:
			return fwd_call_str(client, pProp, old_pData, new_pData, objectID);
			case prop_types::tstring:
			return fwd_call_tstr(client, pProp, old_pData, new_pData, objectID);
		}
		return false;
	}

	void proxy_call(const SendProp *pProp, const void *pStructBase, const void *pOldData, const void *pNewData, DVariant *pOut, int iElement, int objectID) const noexcept
	{
	#if SOURCE_ENGINE == SE_TF2
		if(is_prop_cond(pProp)) {
			DVariant ignore{};
			restore->pRealProxy(nullptr, nullptr, pOldData, &ignore, -1, -1);
			std_proxies->m_UInt32ToInt32(pProp, pStructBase, pNewData, pOut, iElement, objectID);
		} else
	#endif
		{
			restore->pRealProxy(pProp, pStructBase, pNewData, pOut, iElement, objectID);
		}
	}

	inline callback_t(callback_t &&other) noexcept
		: prop_reference_t{std::move(other)}
	{ operator=(std::move(other)); }

	callback_t &operator=(callback_t &&other) noexcept
	{
		fwd = other.fwd;
		other.fwd = nullptr;
		prop = other.prop;
		other.prop = nullptr;
		offset = other.offset;
		type = other.type;
		ref = other.ref;
		other.ref = INVALID_EHANDLE_INDEX;
		name = std::move(other.name);
		element = other.element;
		other.element = 0;
		per_client_funcs = std::move(other.per_client_funcs);
		return *this;
	}

	IChangeableForward *fwd{nullptr};
	std::size_t offset{-1};
	prop_types type{prop_types::unknown};
	int element{0};
	std::string name{};
	SendProp *prop{nullptr};
	int ref{INVALID_EHANDLE_INDEX};

	struct per_client_func_t
	{
		IPluginFunction *func{nullptr};

		inline per_client_func_t(IPluginFunction *func_) noexcept
			: func{func_}
		{
		}

		inline per_client_func_t(per_client_func_t &&other) noexcept
		{ operator=(std::move(other)); }

		per_client_func_t &operator=(per_client_func_t &&other) noexcept
		{
			func = other.func;
			other.func = nullptr;
			return *this;
		}

	private:
		per_client_func_t(const per_client_func_t &) = delete;
		per_client_func_t &operator=(const per_client_func_t &) = delete;
		per_client_func_t() = delete;
	};

	using per_client_funcs_t = std::vector<per_client_func_t>;
	per_client_funcs_t per_client_funcs{};

private:
	callback_t(const callback_t &) = delete;
	callback_t &operator=(const callback_t &) = delete;
	callback_t() = delete;
};

using callbacks_t = std::unordered_map<const SendProp *, callback_t>;

struct proxyhook_t final
{
	callbacks_t callbacks;
	int ref{INVALID_EHANDLE_INDEX};

	inline proxyhook_t(int ref_) noexcept
		: ref{ref_}
	{
	}

	inline ~proxyhook_t() noexcept
	{
	}

	void add_callback(SendProp *pProp, std::string &&name, int element, prop_types type, int offset, IPluginFunction *func, bool per_client) noexcept
	{
		callbacks_t::iterator it_callback{callbacks.find(pProp)};
		if(it_callback == callbacks.end()) {
			it_callback = callbacks.emplace(std::pair<const SendProp *, callback_t>{pProp, callback_t{ref, pProp, std::move(name), element, type, offset}}).first;
		}

		it_callback->second.add_function(func, per_client);
	}

	inline proxyhook_t(proxyhook_t &&other) noexcept
	{ operator=(std::move(other)); }

	proxyhook_t &operator=(proxyhook_t &&other) noexcept
	{
		callbacks = std::move(other.callbacks);
		ref = other.ref;
		other.ref = INVALID_EHANDLE_INDEX;
		return *this;
	}

private:
	proxyhook_t(const proxyhook_t &) = delete;
	proxyhook_t &operator=(const proxyhook_t &) = delete;
	proxyhook_t() = delete;
};

using hooks_t = std::unordered_map<int, proxyhook_t>;
static hooks_t hooks;

DETOUR_DECL_STATIC6(SendTable_Encode, bool, const SendTable *, pTable, const void *, pStruct, bf_write *, pOut, int, objectID, CUtlMemory<CSendProxyRecipients> *, pRecipients, bool, bNonZeroOnly)
{
	do_calc_delta = false;

	if(!packentity_params || !in_compute_packs) {
		return DETOUR_STATIC_CALL(SendTable_Encode)(pTable, pStruct, pOut, objectID, pRecipients, bNonZeroOnly);
	}

	{
		sendproxy_client_slot = -1;
		if(!DETOUR_STATIC_CALL(SendTable_Encode)(pTable, pStruct, pOut, objectID, pRecipients, bNonZeroOnly)) {
			Host_Error( "SV_PackEntity: SendTable_Encode returned false (ent %d).\n", objectID );
			return false;
		}
	}

	int ref{gamehelpers->IndexToReference(objectID)};

	const std::vector<int> &entities{packentity_params->entities};
	if(std::find(entities.cbegin(), entities.cend(), ref) != entities.cend()) {
		const std::size_t slots_size{packentity_params->slots.size()};
		for(int i{0}; i < slots_size; ++i) {
			std::vector<packed_entity_data_t> &vec{packentity_params->entity_data[i]};
			vec.emplace_back();
			packed_entity_data_t &packedData{vec.back()};

			packedData.ref = ref;
			packedData.allocate();

			sendproxy_client_slot = packentity_params->slots[i];
			const bool encoded{DETOUR_STATIC_CALL(SendTable_Encode)(pTable, pStruct, packedData.writeBuf, objectID, pRecipients, bNonZeroOnly)};
			sendproxy_client_slot = -1;
			if(!encoded) {
				Host_Error( "SV_PackEntity: SendTable_Encode returned false (ent %d).\n", objectID );
				return false;
			}
		}
		do_calc_delta = true;
	}

	return true;
}

DETOUR_DECL_STATIC8(SendTable_CalcDelta, int, const SendTable *, pTable, const void *, pFromState, const int, nFromBits, const void *, pToState, const int, nToBits, int *, pDeltaProps, int, nMaxDeltaProps, const int, objectID)
{
	if(!packentity_params || !in_compute_packs || !do_calc_delta) {
		return DETOUR_STATIC_CALL(SendTable_CalcDelta)(pTable, pFromState, nFromBits, pToState, nToBits, pDeltaProps, nMaxDeltaProps, objectID);
	}

	do_calc_delta = false;

	int global_nChanges{DETOUR_STATIC_CALL(SendTable_CalcDelta)(pTable, pFromState, nFromBits, pToState, nToBits, pDeltaProps, nMaxDeltaProps, objectID)};
	int total_nChanges{global_nChanges};

	if(total_nChanges < nMaxDeltaProps) {
		int *client_deltaProps{new int[nMaxDeltaProps]};

		int new_nChanges{total_nChanges};

		int ref = gamehelpers->IndexToReference(objectID);

		const std::size_t slots_size{packentity_params->slots.size()};
		for(int i{0}; i < slots_size; ++i) {
			using entity_data_t = std::vector<packed_entity_data_t>;
			entity_data_t &entity_data{packentity_params->entity_data[i]};

			packed_entity_data_t *packedData{nullptr};
			for(entity_data_t::reverse_iterator it{entity_data.rbegin()}; it != entity_data.rend(); ++it) {
				if(it->ref == ref) {
					packedData = &(*it);
					break;
				}
			}

			if(!packedData || !packedData->written()) {
				continue;
			}

			const int client_nChanges{DETOUR_STATIC_CALL(SendTable_CalcDelta)(pTable, pFromState, nFromBits, packedData->packedData, packedData->writeBuf->GetNumBitsWritten(), client_deltaProps, nMaxDeltaProps, objectID)};
			int client_nChanges_new{0};

			bool done{false};

			for(int j{0}; j < client_nChanges; ++j) {
				bool found{false};
				for(int k{0}; k < new_nChanges; ++k) {
					if(pDeltaProps[k] == client_deltaProps[j]) {
						found = true;
						break;
					}
				}
				if(!found) {
					++client_nChanges_new;
					pDeltaProps[total_nChanges++] = client_deltaProps[j];
					if(total_nChanges >= nMaxDeltaProps) {
						done = true;
						break;
					}
				}
			}

			if(done) {
				break;
			}

			new_nChanges += client_nChanges_new;
		}

		delete[] client_deltaProps;
	}

	if(total_nChanges > nMaxDeltaProps) {
		total_nChanges = nMaxDeltaProps;
	}

	return total_nChanges;
}

class CFrameSnapshot
{
public:
	CInterlockedInt			m_ListIndex;	// Index info CFrameSnapshotManager::m_FrameSnapshots.

	// Associated frame. 
	int						m_nTickCount; // = sv.tickcount
	
	// State information
	class CFrameSnapshotEntry		*m_pEntities;	
	int						m_nNumEntities; // = sv.num_edicts

	// This list holds the entities that are in use and that also aren't entities for inactive clients.
	unsigned short			*m_pValidEntities; 
	int						m_nValidEntities;

	// Additional HLTV info
	class CHLTVEntityData			*m_pHLTVEntityData; // is NULL if not in HLTV mode or array of m_pValidEntities entries
	class CReplayEntityData		*m_pReplayEntityData; // is NULL if not in replay mode or array of m_pValidEntities entries

	class CEventInfo				**m_pTempEntities; // temp entities
	int						m_nTempEntities;

	CUtlVector<int>			m_iExplicitDeleteSlots;

private:

	// Snapshots auto-delete themselves when their refcount goes to zero.
	CInterlockedInt			m_nReferences;
};

DETOUR_DECL_MEMBER2(CFrameSnapshotManager_GetPackedEntity, PackedEntity *, CFrameSnapshot *, pSnapshot, int, entity)
{
	if(!pSnapshot || !packentity_params || !writedeltaentities_client || packentity_params->snapshot_index != pSnapshot->m_ListIndex) {
		return DETOUR_MEMBER_CALL(CFrameSnapshotManager_GetPackedEntity)(pSnapshot, entity);
	}

	PackedEntity *packed{DETOUR_MEMBER_CALL(CFrameSnapshotManager_GetPackedEntity)(pSnapshot, entity)};
	if(!packed) {
		return nullptr;
	}

	const int slot{writedeltaentities_client};

	int ref{gamehelpers->IndexToReference(entity)};

	const packed_entity_data_t *packedData{nullptr};
	const std::size_t slots_size{packentity_params->slots.size()};
	for(int i{0}; i < slots_size; ++i) {
		if(packentity_params->slots[i] == slot) {
			const std::vector<packed_entity_data_t> &entity_data{packentity_params->entity_data[i]};
			for(const packed_entity_data_t &it : entity_data) {
				if(it.ref == ref) {
					if(it.written()) {
						packedData = &it;
					}
					break;
				}
			}
			break;
		}
	}

	if(packedData) {
		if(packed->GetData()) {
			packed->FreeData();
		}

		packed->AllocAndCopyPadded(packedData->packedData, packedData->writeBuf->GetNumBytesWritten());
	}

	return packed;
}

static ConVar *sv_stressbots{nullptr};

static bool is_client_valid(CBaseClient *client) noexcept
{
	if(!sv_stressbots->GetBool()) {
		if(client->IsFakeClient() ||
			client->IsHLTV() ||
			client->IsReplay()) {
			return false;
		}
	}

	if(!client->IsConnected() ||
		!client->IsSpawned() ||
		!client->IsActive()) {
		return false;
	}

	return true;
}

#include <iserver.h>

class CBaseServer : public IServer
{
public:
	virtual float	GetCPUUsage( void ) = 0;
	virtual void	BroadcastPrintf ( PRINTF_FORMAT_STRING const char *fmt, ...) = 0;
	virtual void	SetMaxClients( int number ) = 0;
	virtual void	WriteDeltaEntities( CBaseClient *client, CClientFrame *to, CClientFrame *from,	bf_write &pBuf ) = 0;
};

void PreWriteDeltaEntities(CBaseClient *client)
{
	if(do_writedelta_entities) {
		if(!is_client_valid(client)) {
			writedeltaentities_client = -1;
		} else {
			writedeltaentities_client = client->GetPlayerSlot();
		}
	}

	for(auto it : g_Sample.pack_ent_listeners) {
		it->pre_write_deltas();
	}
}

void PostWriteDeltaEntities()
{
	for(auto it : g_Sample.pack_ent_listeners) {
		it->post_write_deltas();
	}

	if(do_writedelta_entities) {
		writedeltaentities_client = -1;
	}
}

DETOUR_DECL_MEMBER4(CBaseServer_WriteDeltaEntities, void, CBaseClient *, client, CClientFrame *, to, CClientFrame *, from, bf_write &, pBuf)
{
	PreWriteDeltaEntities(client);
	DETOUR_MEMBER_CALL(CBaseServer_WriteDeltaEntities)(client, to, from, pBuf);
	PostWriteDeltaEntities();
}

static CDetour *CBaseServer_WriteDeltaEntities_detour{nullptr};

static ConVar *sv_parallel_packentities{nullptr};
static ConVar *sv_parallel_sendsnapshot{nullptr};

DETOUR_DECL_STATIC0(InvalidateSharedEdictChangeInfos, void)
{
	DETOUR_STATIC_CALL(InvalidateSharedEdictChangeInfos)();
}

static std::thread::id main_thread_id;

struct PackWork_t;

DETOUR_DECL_STATIC1(PackWork_tProcess, void, PackWork_t &, item)
{
	DETOUR_STATIC_CALL(PackWork_tProcess)(item);
}

DETOUR_DECL_STATIC4(SV_PackEntity, void, int, edictIdx, edict_t *, edict, ServerClass *, pServerClass, CFrameSnapshot *, pSnapshot)
{
	DETOUR_STATIC_CALL(SV_PackEntity)(edictIdx, edict, pServerClass, pSnapshot);
}

static CDetour *SendTable_CalcDelta_detour{nullptr};
static CDetour *SendTable_Encode_detour{nullptr};
static CDetour *CFrameSnapshotManager_GetPackedEntity_detour{nullptr};

DETOUR_DECL_STATIC3(SV_ComputeClientPacks, void, int, clientCount, CGameClient **, clients, CFrameSnapshot *, snapshot)
{
	packentity_params.reset(nullptr);

	std::vector<int> slots{};

	for(int i{0}; i < clientCount; ++i) {
		CGameClient *client{clients[i]};
		if(!is_client_valid(client)) {
			continue;
		}
		slots.emplace_back(client->GetPlayerSlot());
	}
	const std::size_t slots_size{slots.size()};

	std::vector<int> entities{};

	bool any_hook{false};

	const hooks_t &chooks{hooks};

	for(int i{0}; i < snapshot->m_nValidEntities; ++i) {
		int idx{snapshot->m_pValidEntities[i]};
		int ref{gamehelpers->IndexToReference(idx)};

		for(auto it : g_Sample.pack_ent_listeners) {
			CBaseEntity *pEntity{gamehelpers->ReferenceToEntity(ref)};
			if(pEntity) {
				it->pre_pack_entity(pEntity);
			}
		}

		hooks_t::const_iterator it_hook{chooks.find(ref)};
		if(it_hook != chooks.cend()) {
			if(!it_hook->second.callbacks.empty()) {
				any_hook = true;
			}
			if(slots_size > 0) {
				bool any_per_client_func{false};
				for(const auto &it_callback : it_hook->second.callbacks) {
					if(it_callback.second.has_any_per_client_func()) {
						any_per_client_func = true;
						break;
					}
				}
				if(any_per_client_func) {
					entities.emplace_back(ref);
				}
			}
		}
	}

	const bool any_per_client_hook{slots_size > 0 && entities.size() > 0};

#if defined _DEBUG && 0
	printf("slots = %i, entities = %i\n", slots.size(), entities.size());
	printf("any_hook = %i, any_per_client_hook = %i, is_parallel_pack_allowed = %i\n", any_hook, any_per_client_hook, g_Sample.is_parallel_pack_allowed());
#endif

	if(any_per_client_hook) {
		packentity_params.reset(new pack_entity_params_t{std::move(slots), std::move(entities), snapshot->m_ListIndex});
		SendTable_Encode_detour->EnableDetour();
		SendTable_CalcDelta_detour->EnableDetour();
		CFrameSnapshotManager_GetPackedEntity_detour->EnableDetour();
		do_writedelta_entities = true;
	} else {
		do_writedelta_entities = false;
		CFrameSnapshotManager_GetPackedEntity_detour->DisableDetour();
		SendTable_Encode_detour->DisableDetour();
		SendTable_CalcDelta_detour->DisableDetour();
	}

	const bool parallel_pack{
		!any_hook &&
		g_Sample.is_parallel_pack_allowed()
	};

	sv_parallel_sendsnapshot->SetValue(true);
	sv_parallel_packentities->SetValue(parallel_pack);

	in_compute_packs = true;
	DETOUR_STATIC_CALL(SV_ComputeClientPacks)(clientCount, clients, snapshot);
	in_compute_packs = false;

	if(!sv_parallel_packentities->GetBool() && any_per_client_hook) {
		SendTable_Encode_detour->DisableDetour();
		SendTable_CalcDelta_detour->DisableDetour();
	}
}

bool Sample::is_parallel_pack_allowed() const noexcept
{
	return !std::any_of(pack_ent_listeners.cbegin(), pack_ent_listeners.cend(), 
		[](const parallel_pack_listener *listener) noexcept -> bool {
			return !listener->is_allowed();
		}
	);
}

bool Sample::add_listener(const parallel_pack_listener *ptr) noexcept
{
	if(std::find(pack_ent_listeners.cbegin(), pack_ent_listeners.cend(), ptr) != pack_ent_listeners.cend()) {
		return false;
	}

	pack_ent_listeners.emplace_back(ptr);
	return true;
}

bool Sample::remove_listener(const parallel_pack_listener *ptr) noexcept
{
	pack_ent_listeners_t::const_iterator it{std::find(pack_ent_listeners.cbegin(), pack_ent_listeners.cend(), ptr)};
	if(it == pack_ent_listeners.cend()) {
		return false;
	}

	pack_ent_listeners.erase(it);
	return true;
}

static void global_send_proxy(const SendProp *pProp, const void *pStructBase, const void *pData, DVariant *pOut, int iElement, int objectID)
{
	proxyrestore_t *restore{nullptr};

	if(objectID != -1) {
		const hooks_t &chooks{hooks};
		int ref{gamehelpers->IndexToReference(objectID)};
		hooks_t::const_iterator it_hook{chooks.find(ref)};
		if(it_hook != chooks.cend()) {
			callbacks_t::const_iterator it_callback{it_hook->second.callbacks.find(pProp)};
			if(it_callback != it_hook->second.callbacks.cend()) {
				restore = it_callback->second.restore;
				const int client{callback_t::get_current_client_entity()};
				if(it_callback->second.can_call_fwd(client)) {
					if(std::this_thread::get_id() == main_thread_id) {
						opaque_ptr new_data{};
						if(it_callback->second.fwd_call(client, pProp, pData, new_data, objectID)) {
							it_callback->second.proxy_call(pProp, pStructBase, pData, new_data.get(), pOut, iElement, objectID);
							return;
						}
					}
				}
			}
		}
	}

	if(!restore) {
		const restores_t &crestores{restores};
		restores_t::const_iterator it_restore{crestores.find(const_cast<SendProp *>(pProp))};
		if(it_restore != crestores.cend()) {
			restore = it_restore->second.get();
		}
	}

	if(restore) {
		restore->pRealProxy(pProp, pStructBase, pData, pOut, iElement, objectID);
	}
}

static void game_frame(bool simulating) noexcept
{
	if(!simulating) {
		return;
	}

	
}

DETOUR_DECL_MEMBER1(CGameServer_SendClientMessages, void, bool, bSendSnapshots)
{
	DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(bSendSnapshots);

	if(!sv_parallel_sendsnapshot->GetBool()) {
		do_writedelta_entities = false;
		CFrameSnapshotManager_GetPackedEntity_detour->DisableDetour();
	}

	packentity_params.reset(nullptr);
}

struct sm_sendprop_info_ex_t final : sm_sendprop_info_t
{
	SendTable *table;
};

static int utlVecOffsetOffset{-1};

static bool UTIL_FindInSendTable(SendTable *pTable, 
						  const char *name,
						  sm_sendprop_info_ex_t *info,
						  unsigned int offset) noexcept
{
	int props = pTable->GetNumProps();
	for (int i = 0; i < props; ++i)
	{
		SendProp *prop = pTable->GetProp(i);

		// Skip InsideArray props (SendPropArray / SendPropArray2),
		// we'll find them later by their containing array.
		if (prop->IsInsideArray()) {
			continue;
		}

		const char *pname = prop->GetName();
		SendTable *pInnerTable = prop->GetDataTable();

		if (pname && strcmp(name, pname) == 0)
		{
			// get true offset of CUtlVector
			if (utlVecOffsetOffset != -1 && prop->GetOffset() == 0 && pInnerTable && pInnerTable->GetNumProps())
			{
				SendProp *pLengthProxy = pInnerTable->GetProp(0);
				const char *ipname = pLengthProxy->GetName();
				if (ipname && strcmp(ipname, "lengthproxy") == 0 && pLengthProxy->GetExtraData())
				{
					info->table = pTable;
					info->prop = prop;
					info->actual_offset = offset + *reinterpret_cast<size_t *>(reinterpret_cast<intptr_t>(pLengthProxy->GetExtraData()) + utlVecOffsetOffset);
					return true;
				}
			}
			info->table = pTable;
			info->prop = prop;
			info->actual_offset = offset + info->prop->GetOffset();
			return true;
		}
		if (pInnerTable)
		{
			if (UTIL_FindInSendTable(pInnerTable, 
				name,
				info,
				offset + prop->GetOffset())
				)
			{
				return true;
			}
		}
	}

	return false;
}

using propinfo_t = std::unordered_map<std::string, sm_sendprop_info_ex_t>;
using propinfos_t = std::unordered_map<ServerClass *, propinfo_t>;
static propinfos_t propinfos;

bool Sample::remove_serverclass_from_cache(ServerClass *pClass) noexcept
{
	propinfos_t::iterator it_props{propinfos.find(pClass)};
	if(it_props == propinfos.cend()) {
		return false;
	}

	propinfos.erase(it_props);
	return true;
}

static bool FindSendPropInfo(ServerClass *pClass, std::string &&name, sm_sendprop_info_ex_t *info) noexcept
{
	propinfos_t::iterator it_props{propinfos.find(pClass)};
	if(it_props == propinfos.cend()) {
		it_props = propinfos.emplace(std::pair<ServerClass *, propinfo_t>{pClass, propinfo_t{}}).first;
	}
	if(it_props != propinfos.cend()) {
		propinfo_t::iterator it_prop{it_props->second.find(name)};
		if(it_prop == it_props->second.cend()) {
			if(UTIL_FindInSendTable(pClass->m_pTable, name.c_str(), info, 0)) {
				it_prop = it_props->second.emplace(std::pair<std::string, sm_sendprop_info_ex_t>{std::move(name), std::move(*info)}).first;
			}
		}
		if(it_prop != it_props->second.cend()) {
			*info = it_prop->second;
			return true;
		}
	}
	return false;
}

static cell_t proxysend_handle_hook(IPluginContext *pContext, hooks_t::iterator it_hook, int idx, int offset, SendProp *pProp, std::string &&prop_name, int element, SendTable *pTable, IPluginFunction *callback, bool per_client)
{
	prop_types type{prop_types::unknown};
	restores_t::const_iterator it_restore{restores.find(pProp)};
	if(it_restore != restores.cend()) {
		type = it_restore->second->type;
	} else {
		type = guess_prop_type(pProp, pTable);
	}
	if(type == prop_types::unknown) {
		return pContext->ThrowNativeError("Unsupported prop");
	}

#ifdef _DEBUG
	printf("added %s %p hook for %i\n", pProp->GetName(), pProp, idx);
#endif

	it_hook->second.add_callback(pProp, std::move(prop_name), element, type, offset, callback, per_client);

	return 0;
}

static cell_t proxysend_hook(IPluginContext *pContext, const cell_t *params) noexcept
{
	CBaseEntity *pEntity{gamehelpers->ReferenceToEntity(params[1])};
	if(!pEntity) {
		return pContext->ThrowNativeError("Invalid Entity Reference/Index %i", params[1]);
	}

	char *name_ptr;
	pContext->LocalToString(params[2], &name_ptr);
	std::string name{name_ptr};

	IPluginFunction *callback{pContext->GetFunctionById(params[3])};

	bool per_client = static_cast<bool>(params[4]);

	IServerNetworkable *pNetwork{pEntity->GetNetworkable()};
	ServerClass *pServer{pNetwork->GetServerClass()};

	sm_sendprop_info_ex_t info{};
	if(!FindSendPropInfo(pServer, std::move(name), &info)) {
		return pContext->ThrowNativeError("Could not find prop %s", name.c_str());
	}
	SendTable *pTable{info.table};

	SendProp *pProp{info.prop};
	std::string prop_name{pProp->GetName()};

	int ref = gamehelpers->EntityToReference(pEntity);

	hooks_t::iterator it_hook{hooks.find(ref)};
	if(it_hook == hooks.end()) {
		it_hook = hooks.emplace(std::pair<int, proxyhook_t>{ref, proxyhook_t{ref}}).first;
	}

	if(pProp->GetType() == DPT_DataTable) {
		SendTable *pPropTable{pProp->GetDataTable()};
		int NumProps{pPropTable->GetNumProps()};
		for(int i = 0; i < NumProps; ++i) {
			SendProp *pChildProp{pPropTable->GetProp(i)};
			std::string tmp_name{prop_name};
			cell_t ret{proxysend_handle_hook(pContext, it_hook, ref, info.actual_offset + pChildProp->GetOffset(), pChildProp, std::move(tmp_name), i, pTable, callback, per_client)};
			if(ret != 0) {
				return ret;
			}
		}
		return 0;
	}

	return proxysend_handle_hook(pContext, it_hook, ref, info.actual_offset, pProp, std::move(prop_name), 0, pTable, callback, per_client);
}

static void proxysend_handle_unhook(hooks_t::iterator it_hook, int ref, const SendProp *pProp, const char *name, IPluginFunction *callback)
{
	callbacks_t::iterator it_callback{it_hook->second.callbacks.find(pProp)};
	if(it_callback != it_hook->second.callbacks.end()) {
		it_callback->second.remove_function(callback);
	#ifdef _DEBUG
		printf("removed func from %s %p callback for %i\n", name, pProp, ref);
	#endif
		if(it_callback->second.fwd->GetFunctionCount() == 0) {
		#ifdef _DEBUG
			printf("removed callback %s %p for %i\n", name, pProp, ref);
		#endif
			it_hook->second.callbacks.erase(it_callback);
		}
	}
}

static cell_t proxysend_unhook(IPluginContext *pContext, const cell_t *params) noexcept
{
	CBaseEntity *pEntity{gamehelpers->ReferenceToEntity(params[1])};
	if(!pEntity) {
		return pContext->ThrowNativeError("Invalid Entity Reference/Index %i", params[1]);
	}

	IServerNetworkable *pNetwork{pEntity->GetNetworkable()};
	ServerClass *pServer{pNetwork->GetServerClass()};

	char *name_ptr;
	pContext->LocalToString(params[2], &name_ptr);

	sm_sendprop_info_t info{};
	if(!gamehelpers->FindSendPropInfo(pServer->GetName(), name_ptr, &info)) {
		return pContext->ThrowNativeError("Could not find prop %s", name_ptr);
	}

	const SendProp *pProp{info.prop};

	IPluginFunction *callback{pContext->GetFunctionById(params[3])};

	int ref = gamehelpers->EntityToReference(pEntity);

	hooks_t::iterator it_hook{hooks.find(ref)};
	if(it_hook != hooks.end()) {
		if(pProp->GetType() == DPT_DataTable) {
			SendTable *pPropTable{pProp->GetDataTable()};
			int NumProps{pPropTable->GetNumProps()};
			for(int i = 0; i < NumProps; ++i) {
				SendProp *pChildProp{pPropTable->GetProp(i)};
				proxysend_handle_unhook(it_hook, ref, pChildProp, name_ptr, callback);
			}
		} else {
			proxysend_handle_unhook(it_hook, ref, pProp, name_ptr, callback);
		}
		if(it_hook->second.callbacks.empty()) {
			hooks.erase(it_hook);
		}
	}

	return 0;
}

static constexpr const sp_nativeinfo_t natives[]{
	{"proxysend_hook", proxysend_hook},
	{"proxysend_unhook", proxysend_unhook},
	{nullptr, nullptr}
};

static CDetour *SV_ComputeClientPacks_detour{nullptr};
static CDetour *SV_PackEntity_detour{nullptr};
static CDetour *PackWork_tProcess_detour{nullptr};
static CDetour *InvalidateSharedEdictChangeInfos_detour{nullptr};
static CDetour *CGameServer_SendClientMessages_detour{nullptr};

bool Sample::SDK_OnLoad(char *error, size_t maxlen, bool late) noexcept
{
	if(!gameconfs->LoadGameConfigFile("proxysend", &gameconf, error, maxlen)) {
		return false;
	}

	IGameConfig *coregameconf{nullptr};
	if(!gameconfs->LoadGameConfigFile("core.games/common.games", &coregameconf, error, maxlen)) {
		return false;
	}

	coregameconf->GetOffset("CSendPropExtra_UtlVector::m_Offset", &utlVecOffsetOffset);

	gameconfs->CloseGameConfigFile(coregameconf);

	gameconf->GetOffset("CBaseClient::UpdateSendState", &CBaseClient_UpdateSendState_idx);
	if(CBaseClient_UpdateSendState_idx == -1) {
		snprintf(error, maxlen, "could not get CBaseClient::UpdateSendState offset");
		return false;
	}

	gameconf->GetOffset("CBaseClient::SendSnapshot", &CBaseClient_SendSnapshot_idx);
	if(CBaseClient_SendSnapshot_idx == -1) {
		snprintf(error, maxlen, "could not get CBaseClient::SendSnapshot offset");
		return false;
	}

	gameconf->GetMemSig("CGameClient::GetSendFrame", &CGameClient_GetSendFrame_ptr);
	if(CGameClient_GetSendFrame_ptr == nullptr) {
		snprintf(error, maxlen, "could not get CGameClient::GetSendFrame address");
		return false;
	}

	CDetourManager::Init(smutils->GetScriptingEngine(), gameconf);

	SendTable_CalcDelta_detour = DETOUR_CREATE_STATIC(SendTable_CalcDelta, "SendTable_CalcDelta");
	if(!SendTable_CalcDelta_detour) {
		snprintf(error, maxlen, "could not create SendTable_CalcDelta detour");
		return false;
	}

	SendTable_Encode_detour = DETOUR_CREATE_STATIC(SendTable_Encode, "SendTable_Encode");
	if(!SendTable_Encode_detour) {
		snprintf(error, maxlen, "could not create SendTable_Encode detour");
		return false;
	}

	SV_ComputeClientPacks_detour = DETOUR_CREATE_STATIC(SV_ComputeClientPacks, "SV_ComputeClientPacks");
	if(!SV_ComputeClientPacks_detour) {
		snprintf(error, maxlen, "could not create SV_ComputeClientPacks detour");
		return false;
	}

	CGameServer_SendClientMessages_detour = DETOUR_CREATE_MEMBER(CGameServer_SendClientMessages, "CGameServer::SendClientMessages");
	if(!CGameServer_SendClientMessages_detour) {
		snprintf(error, maxlen, "could not create CGameServer::SendClientMessages detour");
		return false;
	}

	CFrameSnapshotManager_GetPackedEntity_detour = DETOUR_CREATE_MEMBER(CFrameSnapshotManager_GetPackedEntity, "CFrameSnapshotManager::GetPackedEntity");
	if(!CFrameSnapshotManager_GetPackedEntity_detour) {
		snprintf(error, maxlen, "could not create CFrameSnapshotManager::GetPackedEntity detour");
		return false;
	}

#if SOURCE_ENGINE == SE_TF2
	{
		void **vtable = *(void ***)server;
		int index = vfunc_index(&CBaseServer::WriteDeltaEntities);
		CBaseServer_WriteDeltaEntities_detour = DETOUR_CREATE_MEMBER(CBaseServer_WriteDeltaEntities, vtable[index]);
		CBaseServer_WriteDeltaEntities_detour->EnableDetour();
	}
#endif

#if 0
	//SV_PackEntity_detour = DETOUR_CREATE_STATIC(SV_PackEntity, "SV_PackEntity");
	//SV_PackEntity_detour->EnableDetour();

	//PackWork_tProcess_detour = DETOUR_CREATE_STATIC(PackWork_tProcess, "PackWork_t::Process");
	//PackWork_tProcess_detour->EnableDetour();

	//InvalidateSharedEdictChangeInfos_detour = DETOUR_CREATE_STATIC(InvalidateSharedEdictChangeInfos, "InvalidateSharedEdictChangeInfos");
	//InvalidateSharedEdictChangeInfos_detour->EnableDetour();
#endif

	CGameServer_SendClientMessages_detour->EnableDetour();
	SV_ComputeClientPacks_detour->EnableDetour();

	g_pEntityList = reinterpret_cast<CBaseEntityList *>(gamehelpers->GetGlobalEntityList());

	sharesys->AddDependency(myself, "sdkhooks.ext", true, true);

	sharesys->AddInterface(myself, this);
	sharesys->RegisterLibrary(myself, "proxysend");

	smutils->AddGameFrameHook(game_frame);
	plsys->AddPluginsListener(this);

	sharesys->AddNatives(myself, natives);

#if SOURCE_ENGINE == SE_TF2
	sm_sendprop_info_t info{};
	gamehelpers->FindSendPropInfo("CTFPlayer", "m_nPlayerCond", &info);
	m_nPlayerCond = info.prop;
	gamehelpers->FindSendPropInfo("CTFPlayer", "_condition_bits", &info);
	_condition_bits = info.prop;
	gamehelpers->FindSendPropInfo("CTFPlayer", "m_nPlayerCondEx", &info);
	m_nPlayerCondEx = info.prop;
	gamehelpers->FindSendPropInfo("CTFPlayer", "m_nPlayerCondEx2", &info);
	m_nPlayerCondEx2 = info.prop;
	gamehelpers->FindSendPropInfo("CTFPlayer", "m_nPlayerCondEx3", &info);
	m_nPlayerCondEx3 = info.prop;
	gamehelpers->FindSendPropInfo("CTFPlayer", "m_nPlayerCondEx4", &info);
	m_nPlayerCondEx4 = info.prop;
#endif

	return true;
}

bool Sample::RegisterConCommandBase(ConCommandBase *pCommand)
{
	META_REGCVAR(pCommand);
	return true;
}

bool Sample::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late) noexcept
{
	GET_V_IFACE_ANY(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	ConVar_Register(0, this);

#if SOURCE_ENGINE == SE_TF2
	server = engine->GetIServer();
#endif

	std_proxies = gamedll->GetStandardSendProxies();

	main_thread_id = std::this_thread::get_id();

	sv_parallel_packentities = g_pCVar->FindVar("sv_parallel_packentities");
	sv_parallel_sendsnapshot = g_pCVar->FindVar("sv_parallel_sendsnapshot");
	sv_stressbots = g_pCVar->FindVar("sv_stressbots");

	return true;
}

void Sample::OnCoreMapEnd() noexcept
{
	hooks.clear();
	restores.clear();
}

void Sample::SDK_OnUnload() noexcept
{
	OnCoreMapEnd();

	SendTable_CalcDelta_detour->Destroy();
	SendTable_Encode_detour->Destroy();
	SV_ComputeClientPacks_detour->Destroy();
	//SV_PackEntity_detour->Destroy();
	//PackWork_tProcess_detour->Destroy();
	//InvalidateSharedEdictChangeInfos_detour->Destroy();
	CGameServer_SendClientMessages_detour->Destroy();
	CFrameSnapshotManager_GetPackedEntity_detour->Destroy();
	CBaseServer_WriteDeltaEntities_detour->Destroy();

	gameconfs->CloseGameConfigFile(gameconf);

	smutils->RemoveGameFrameHook(game_frame);
	plsys->RemovePluginsListener(this);
	if(g_pSDKHooks) {
		g_pSDKHooks->RemoveEntityListener(this);
	}
}

void Sample::OnEntityDestroyed(CBaseEntity *pEntity) noexcept
{
	if(!pEntity) {
		return;
	}

	const int ref{gamehelpers->EntityToReference(pEntity)};

	hooks_t::iterator it_hook{hooks.find(ref)};
	if(it_hook != hooks.end()) {
		hooks.erase(it_hook);
	}
}

void Sample::OnPluginUnloaded(IPlugin *plugin) noexcept
{
	hooks_t::iterator it_hook{hooks.begin()};
	while(it_hook != hooks.end()) {
		callbacks_t::iterator it_callback{it_hook->second.callbacks.begin()};
		while(it_callback != it_hook->second.callbacks.end()) {
			it_callback->second.remove_functions_of_plugin(plugin);
			if(it_callback->second.fwd->GetFunctionCount() == 0) {
				it_callback = it_hook->second.callbacks.erase(it_callback);
				continue;
			}
			++it_callback;
		}
		if(it_hook->second.callbacks.empty()) {
			it_hook = hooks.erase(it_hook);
			continue;
		}
		++it_hook;
	}
}

bool Sample::QueryRunning(char *error, size_t maxlength)
{
	SM_CHECK_IFACE(SDKHOOKS, g_pSDKHooks);
	SM_CHECK_IFACE(SDKTOOLS, g_pSDKTools);
	return true;
}

bool Sample::QueryInterfaceDrop(SMInterface *pInterface)
{
	if(pInterface == g_pSDKHooks)
		return false;
	else if(pInterface == g_pSDKTools)
		return false;
	return IExtensionInterface::QueryInterfaceDrop(pInterface);
}

void Sample::NotifyInterfaceDrop(SMInterface *pInterface)
{
	if(strcmp(pInterface->GetInterfaceName(), SMINTERFACE_SDKHOOKS_NAME) == 0) {
		g_pSDKHooks->RemoveEntityListener(this);
		g_pSDKHooks = NULL;
	} else if(strcmp(pInterface->GetInterfaceName(), SMINTERFACE_SDKTOOLS_NAME) == 0) {
		g_pSDKTools = NULL;
	}
}

void Sample::SDK_OnAllLoaded() noexcept
{
	SM_GET_LATE_IFACE(SDKHOOKS, g_pSDKHooks);
	SM_GET_LATE_IFACE(SDKTOOLS, g_pSDKTools);

	g_pSDKHooks->AddEntityListener(this);

#if SOURCE_ENGINE == SE_LEFT4DEAD2
	server = g_pSDKTools->GetIServer();

	{
		void **vtable = *(void ***)server;
		int index = vfunc_index(&CBaseServer::WriteDeltaEntities);
		CBaseServer_WriteDeltaEntities_detour = DETOUR_CREATE_MEMBER(CBaseServer_WriteDeltaEntities, vtable[index]);
		CBaseServer_WriteDeltaEntities_detour->EnableDetour();
	}
#endif
}