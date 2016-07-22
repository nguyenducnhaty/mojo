// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <magenta/syscalls.h>
#include <string.h>

#include "mojo/public/c/system/buffer.h"
#include "mojo/public/c/system/handle.h"
#include "mojo/public/c/system/message_pipe.h"
#include "mojo/public/c/system/wait.h"

// MojoTimeTicks is in microseconds.
// mx_time_t is in nanoseconds.

// TODO(abarth): MojoTimeTicks probably shouldn't be signed.
static MojoTimeTicks TimeToMojoTicks(mx_time_t t) {
  return (MojoTimeTicks)t / 1000u;
}

static mx_time_t MojoDeadlineToTime(MojoDeadline deadline) {
  if (deadline == MOJO_DEADLINE_INDEFINITE)
    return MX_TIME_INFINITE;
  // TODO(abarth): Handle overflow.
  return (mx_time_t)deadline * 1000u;
}

// handle.h --------------------------------------------------------------------

MojoResult MojoClose(MojoHandle handle) {
  mx_status_t status = mx_handle_close((mx_handle_t)handle);
  switch (status) {
    case NO_ERROR:
      return MOJO_RESULT_OK;
    case ERR_INVALID_ARGS:
      return MOJO_RESULT_INVALID_ARGUMENT;
    default:
      return MOJO_RESULT_UNKNOWN;
  }
}

MojoResult MojoDuplicateHandle(MojoHandle handle, MojoHandle* new_handle) {
  mx_handle_t result =
      mx_handle_duplicate((mx_handle_t)handle, MX_RIGHT_SAME_RIGHTS);
  if (result < 0) {
    switch (result) {
      case ERR_BAD_HANDLE:
        return MOJO_RESULT_INVALID_ARGUMENT;
      case ERR_INVALID_ARGS:
        return MOJO_RESULT_INVALID_ARGUMENT;
      case ERR_ACCESS_DENIED:
        return MOJO_RESULT_PERMISSION_DENIED;
      case ERR_NO_MEMORY:
        return MOJO_RESULT_RESOURCE_EXHAUSTED;
      default:
        return MOJO_RESULT_UNKNOWN;
    }
  }
  *new_handle = (MojoHandle)result;
  return MOJO_RESULT_OK;
}

MojoResult MojoGetRights(MojoHandle handle, MojoHandleRights* rights) {
  mx_handle_basic_info_t handle_info;
  mx_ssize_t result =
      mx_handle_get_info((mx_handle_t)handle, MX_INFO_HANDLE_BASIC,
                         &handle_info, sizeof(handle_info));
  if (result < 0) {
    switch (result) {
      case ERR_BUSY:
        return MOJO_RESULT_BUSY;
      case ERR_BAD_HANDLE:
      case ERR_INVALID_ARGS:
        return MOJO_RESULT_INVALID_ARGUMENT;
      case ERR_ACCESS_DENIED:
        return MOJO_RESULT_PERMISSION_DENIED;
      case ERR_NO_MEMORY:
        return MOJO_RESULT_RESOURCE_EXHAUSTED;
      default:
        return MOJO_RESULT_UNKNOWN;
    }
  }
  *rights = handle_info.rights;
  return MOJO_RESULT_OK;
}

MojoResult MojoReplaceHandleWithReducedRights(MojoHandle handle,
                                              MojoHandleRights rights_to_remove,
                                              MojoHandle* replacement_handle) {
  // TODO: This doesn't work for handles without MOJO_HANDLE_RIGHT_DUPLICATE.
  MojoHandle new_handle;
  MojoResult result = MojoDuplicateHandleWithReducedRights(
      handle, rights_to_remove, &new_handle);
  if (result != MOJO_RESULT_OK)
    return result;
  result = MojoClose(handle);
  if (result != MOJO_RESULT_OK)
    return result;
  *replacement_handle = new_handle;
  return MOJO_RESULT_OK;
}

MojoResult MojoDuplicateHandleWithReducedRights(
    MojoHandle handle,
    MojoHandleRights rights_to_remove,
    MojoHandle* new_handle) {
  MojoHandleRights original_rights;
  MojoResult result = MojoGetRights(handle, &original_rights);
  if (result != MOJO_RESULT_OK)
    return result;
  MojoHandleRights new_rights = original_rights & ~rights_to_remove;
  mx_handle_t new_mx_handle =
      mx_handle_duplicate((mx_handle_t)handle, new_rights);
  if (new_mx_handle < 0) {
    switch (new_mx_handle) {
      case ERR_BUSY:
        return MOJO_RESULT_BUSY;
      case ERR_BAD_HANDLE:
      case ERR_INVALID_ARGS:
        return MOJO_RESULT_INVALID_ARGUMENT;
      case ERR_ACCESS_DENIED:
        return MOJO_RESULT_PERMISSION_DENIED;
      case ERR_NO_MEMORY:
        return MOJO_RESULT_RESOURCE_EXHAUSTED;
      default:
        return MOJO_RESULT_UNKNOWN;
    }
  }
  *new_handle = (MojoHandle)new_mx_handle;
  return MOJO_RESULT_OK;
}

// time.h ----------------------------------------------------------------------

MojoTimeTicks MojoGetTimeTicksNow() {
  return TimeToMojoTicks(mx_current_time());
}

// wait.h ----------------------------------------------------------------------

static_assert(MOJO_HANDLE_SIGNAL_NONE == MX_SIGNAL_NONE,
              "SIGNAL_NONE must match");
static_assert(MOJO_HANDLE_SIGNAL_READABLE == MX_SIGNAL_READABLE,
              "SIGNAL_READABLE must match");
static_assert(MOJO_HANDLE_SIGNAL_WRITABLE == MX_SIGNAL_WRITABLE,
              "SIGNAL_WRITABLE must match");
static_assert(MOJO_HANDLE_SIGNAL_PEER_CLOSED == MX_SIGNAL_PEER_CLOSED,
              "PEER_CLOSED must match");
static_assert(sizeof(struct MojoHandleSignalsState) ==
                  sizeof(mx_signals_state_t),
              "Signals state structs must match");

MojoResult MojoWait(MojoHandle handle,
                    MojoHandleSignals signals,
                    MojoDeadline deadline,
                    struct MojoHandleSignalsState* signals_state) {
  mx_time_t mx_deadline = MojoDeadlineToTime(deadline);
  mx_signals_state_t* mx_signals_state = (mx_signals_state_t*)signals_state;

  mx_status_t status = mx_handle_wait_one((mx_handle_t)handle, signals,
                                          mx_deadline, mx_signals_state);

  switch (status) {
    case NO_ERROR:
      return MOJO_RESULT_OK;
    case ERR_CANCELLED:
      return MOJO_RESULT_CANCELLED;
    // TODO(vtl): This is currently not specified for MojoWait(), nor does
    // mx_handle_wait_one() currently return this.
    case ERR_NO_MEMORY:
      return MOJO_RESULT_RESOURCE_EXHAUSTED;
    case ERR_INVALID_ARGS:
      return MOJO_RESULT_INVALID_ARGUMENT;
    case ERR_TIMED_OUT:
      return MOJO_RESULT_DEADLINE_EXCEEDED;
    case ERR_BAD_STATE:
      return MOJO_RESULT_FAILED_PRECONDITION;
    // TODO(vtl): mx_handle_wait_one() currently never returns this.
    case ERR_BUSY:
      return MOJO_RESULT_BUSY;
    // TODO(vtl): The Mojo version doesn't require any rights to wait, whereas
    // Magenta requires MX_RIGHT_READ.
    case ERR_ACCESS_DENIED:
      return MOJO_RESULT_PERMISSION_DENIED;
    default:
      return MOJO_RESULT_UNKNOWN;
  }
}

MojoResult MojoWaitMany(const MojoHandle* handles,
                        const MojoHandleSignals* signals,
                        uint32_t num_handles,
                        MojoDeadline deadline,
                        uint32_t* result_index,
                        struct MojoHandleSignalsState* signals_states) {
  const mx_handle_t* mx_handles = (const mx_handle_t*)handles;
  const mx_signals_t* mx_signals = (const mx_signals_t*)signals;
  mx_time_t mx_deadline = MojoDeadlineToTime(deadline);
  mx_signals_state_t* mx_signals_state = (mx_signals_state_t*)signals_states;

  mx_status_t status =
      mx_handle_wait_many(num_handles, mx_handles, mx_signals, mx_deadline,
                          result_index, mx_signals_state);

  switch (status) {
    case NO_ERROR:
      return MOJO_RESULT_OK;
    case ERR_CANCELLED:
      return MOJO_RESULT_CANCELLED;
    case ERR_NO_MEMORY:
      return MOJO_RESULT_RESOURCE_EXHAUSTED;
    case ERR_INVALID_ARGS:
      return MOJO_RESULT_INVALID_ARGUMENT;
    case ERR_TIMED_OUT:
      return MOJO_RESULT_DEADLINE_EXCEEDED;
    case ERR_BAD_STATE:
      return MOJO_RESULT_FAILED_PRECONDITION;
    // TODO(vtl): mx_handle_wait_many() currently never returns this.
    case ERR_BUSY:
      return MOJO_RESULT_BUSY;
    // TODO(vtl): The Mojo version doesn't require any rights to wait, whereas
    // Magenta requires MX_RIGHT_READ.
    case ERR_ACCESS_DENIED:
      return MOJO_RESULT_PERMISSION_DENIED;
    default:
      return MOJO_RESULT_UNKNOWN;
  }
}

// message_pipe.h --------------------------------------------------------------

MojoResult MojoCreateMessagePipe(
    const struct MojoCreateMessagePipeOptions* options,
    MojoHandle* message_pipe_handle0,
    MojoHandle* message_pipe_handle1) {
  if (options && options->flags != MOJO_CREATE_MESSAGE_PIPE_OPTIONS_FLAG_NONE)
    return MOJO_RESULT_INVALID_ARGUMENT;
  mx_handle_t mx_handles[2];
  mx_status_t status = mx_message_pipe_create(mx_handles, 0);
  if (status != NO_ERROR) {
    switch (status) {
      case ERR_INVALID_ARGS:
        return MOJO_RESULT_INVALID_ARGUMENT;
      case ERR_NO_MEMORY:
        return MOJO_RESULT_RESOURCE_EXHAUSTED;
      default:
        return MOJO_RESULT_UNKNOWN;
    }
  }
  *message_pipe_handle0 = (MojoHandle)mx_handles[0];
  *message_pipe_handle1 = (MojoHandle)mx_handles[1];
  return MOJO_RESULT_OK;
}

MojoResult MojoWriteMessage(MojoHandle message_pipe_handle,
                            const void* bytes,
                            uint32_t num_bytes,
                            const MojoHandle* handles,
                            uint32_t num_handles,
                            MojoWriteMessageFlags flags) {
  mx_handle_t* mx_handles = (mx_handle_t*)handles;
  // TODO(abarth): Handle messages that are too big to fit.
  mx_status_t status =
      mx_message_write((mx_handle_t)message_pipe_handle, bytes, num_bytes,
                       mx_handles, num_handles, flags);
  switch (status) {
    case NO_ERROR:
      return MOJO_RESULT_OK;
    case ERR_INVALID_ARGS:
      return MOJO_RESULT_INVALID_ARGUMENT;
    case ERR_ACCESS_DENIED:
      return MOJO_RESULT_PERMISSION_DENIED;
    case ERR_BAD_STATE:
      // Notice the different semantics than mx_message_read.
      return MOJO_RESULT_FAILED_PRECONDITION;
    case ERR_NO_MEMORY:
      return MOJO_RESULT_RESOURCE_EXHAUSTED;
    case ERR_TOO_BIG:
    // TODO(abarth): Handle messages that are too big to fit.
    default:
      return MOJO_RESULT_UNKNOWN;
  }
}

MojoResult MojoReadMessage(MojoHandle message_pipe_handle,
                           void* bytes,
                           uint32_t* num_bytes,
                           MojoHandle* handles,
                           uint32_t* num_handles,
                           MojoReadMessageFlags flags) {
  mx_handle_t* mx_handles = (mx_handle_t*)handles;
  // TODO(abarth): Handle messages that were too big to fit.
  mx_status_t status =
      mx_message_read((mx_handle_t)message_pipe_handle, bytes, num_bytes,
                      mx_handles, num_handles, flags);
  switch (status) {
    case NO_ERROR:
      return MOJO_RESULT_OK;
    case ERR_INVALID_ARGS:
      return MOJO_RESULT_INVALID_ARGUMENT;
    case ERR_ACCESS_DENIED:
      return MOJO_RESULT_PERMISSION_DENIED;
    case ERR_BAD_STATE:
      // Notice the different semantics than mx_message_write.
      return MOJO_RESULT_SHOULD_WAIT;
    case ERR_CHANNEL_CLOSED:
      return MOJO_RESULT_FAILED_PRECONDITION;
    case ERR_NO_MEMORY:
      // Notice the collision with ERR_NOT_ENOUGH_BUFFER.
      return MOJO_RESULT_RESOURCE_EXHAUSTED;
    case ERR_NOT_ENOUGH_BUFFER:
      return MOJO_RESULT_RESOURCE_EXHAUSTED;
    default:
      return MOJO_RESULT_UNKNOWN;
  }
}

// data_pipe.h -----------------------------------------------------------------

// TODO(abarth): Not implemented.

// buffer.h --------------------------------------------------------------------

MojoResult MojoCreateSharedBuffer(
    const struct MojoCreateSharedBufferOptions* options,
    uint64_t num_bytes,
    MojoHandle* shared_buffer_handle) {
  if (options && options->flags != MOJO_CREATE_SHARED_BUFFER_OPTIONS_FLAG_NONE)
    return MOJO_RESULT_UNIMPLEMENTED;
  mx_handle_t result = mx_vm_object_create(num_bytes);
  if (result < 0) {
    switch (result) {
      case ERR_INVALID_ARGS:
        return MOJO_RESULT_INVALID_ARGUMENT;
      case ERR_NO_MEMORY:
        return MOJO_RESULT_RESOURCE_EXHAUSTED;
      default:
        return MOJO_RESULT_UNKNOWN;
    }
  }
  *shared_buffer_handle = (MojoHandle)result;
  return MOJO_RESULT_OK;
}

MojoResult MojoDuplicateBufferHandle(
    MojoHandle buffer_handle,
    const struct MojoDuplicateBufferHandleOptions* options,
    MojoHandle* new_buffer_handle) {
  if (options &&
      options->flags != MOJO_DUPLICATE_BUFFER_HANDLE_OPTIONS_FLAG_NONE)
    return MOJO_RESULT_UNIMPLEMENTED;
  return MojoDuplicateHandle(buffer_handle, new_buffer_handle);
}

MojoResult MojoGetBufferInformation(MojoHandle buffer_handle,
                                    struct MojoBufferInformation* info,
                                    uint32_t info_num_bytes) {
  if (!info || info_num_bytes < 16)
    return MOJO_RESULT_INVALID_ARGUMENT;
  mx_handle_t vmo_handle = (mx_handle_t)buffer_handle;
  uint64_t num_bytes = 0;
  mx_status_t status = mx_vm_object_get_size(vmo_handle, &num_bytes);
  switch (status) {
    case NO_ERROR:
      info->struct_size = sizeof(struct MojoBufferInformation);
      info->flags = MOJO_BUFFER_INFORMATION_FLAG_NONE;
      info->num_bytes = num_bytes;
      return MOJO_RESULT_OK;
    case ERR_INVALID_ARGS:
      return MOJO_RESULT_INVALID_ARGUMENT;
    case ERR_ACCESS_DENIED:
      return MOJO_RESULT_PERMISSION_DENIED;
    default:
      return MOJO_RESULT_UNKNOWN;
  }
}

MojoResult MojoMapBuffer(MojoHandle buffer_handle,
                         uint64_t offset,
                         uint64_t num_bytes,
                         void** buffer,
                         MojoMapBufferFlags flags) {
  if (flags != MOJO_MAP_BUFFER_FLAG_NONE)
    return MOJO_RESULT_INVALID_ARGUMENT;
  mx_handle_t vmo_handle = (mx_handle_t)buffer_handle;
  uintptr_t* mx_pointer = (uintptr_t*)buffer;
  // TODO(abarth): Mojo doesn't let you specify any flags. It's unclear whether
  // this is a reasonable default.
  uint32_t mx_flags = MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE;

  mx_status_t status =
      mx_process_vm_map(0, vmo_handle, offset, num_bytes, mx_pointer, mx_flags);
  switch (status) {
    case NO_ERROR:
      return MOJO_RESULT_OK;
    case ERR_INVALID_ARGS:
      return MOJO_RESULT_INVALID_ARGUMENT;
    case ERR_ACCESS_DENIED:
      return MOJO_RESULT_PERMISSION_DENIED;
    case ERR_NO_MEMORY:
      return MOJO_RESULT_RESOURCE_EXHAUSTED;
    default:
      return MOJO_RESULT_UNKNOWN;
  }
}

MojoResult MojoUnmapBuffer(void* buffer) {
  uintptr_t address = (uintptr_t)buffer;
  // TODO(abarth): mx_process_vm_unmap needs the length to unmap, but Mojo
  // doesn't give us the length.
  mx_size_t length = 0;
  mx_status_t status = mx_process_vm_unmap(0, address, length);
  switch (status) {
    case NO_ERROR:
      return MOJO_RESULT_OK;
    case ERR_INVALID_ARGS:
      return MOJO_RESULT_INVALID_ARGUMENT;
    default:
      return MOJO_RESULT_UNKNOWN;
  }
}

// wait_set.h ------------------------------------------------------------------

// TODO(abarth): Not implemented.
