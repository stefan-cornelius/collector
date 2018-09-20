/** collector

A full notice with attributions is provided along with this source code.

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

* In addition, as a special exception, the copyright holders give
* permission to link the code of portions of this program with the
* OpenSSL library under certain conditions as described in each
* individual source file, and distribute linked combinations
* including the two.
* You must obey the GNU General Public License in all respects
* for all of the code used other than OpenSSL.  If you modify
* file(s) with this exception, you may extend this exception to your
* version of the file(s), but you are not obligated to do so.  If you
* do not wish to do so, delete this exception statement from your
* version.
*/

#ifndef _PROTO_FORMATTER_H_
#define _PROTO_FORMATTER_H_

#include <utility>

#include <google/protobuf/message.h>

#include "ProtoAllocator.h"
#include "SignalFormatter.h"

namespace collector {

// Base class for all signal formatters that output protobuf messages.
class BaseProtoSignalFormatter : public SignalFormatter {
 public:
  BaseProtoSignalFormatter(bool text_format = false) : text_format_(text_format) {}

  bool FormatSignal(SafeBuffer* buf, sinsp_evt* event) override;
  bool FormatSignal(SafeBuffer* buf, sinsp_threadinfo* tinfo) override;

 protected:
  // Returns a pointer to the proto message derived from this event, or nullptr if no message should be output.
  // Note: The caller does not assume ownership of the returned message. To avoid an additional heap allocation, the
  // implementing class should maintain an instance-level message whose address is returned by this method.
  virtual const google::protobuf::Message* ToProtoMessage(sinsp_evt* event) = 0;
  virtual const google::protobuf::Message* ToProtoMessage(sinsp_threadinfo* tinfo) { return NULL; }

  virtual void Reset() = 0;

 private:
  bool MessageToBuf(SafeBuffer* buf,  const google::protobuf::Message* msg);
  bool text_format_;
};

template <typename Message>
class ProtoSignalFormatter : public BaseProtoSignalFormatter, protected ProtoAllocator<Message> {
 public:
  ProtoSignalFormatter(bool text_format = false)
      : BaseProtoSignalFormatter(text_format) {}

 protected:
  void Reset() override { ProtoAllocator<Message>::Reset(); }
};

}  // namespace collector

#endif  // _PROTO_FORMATTER_H_
