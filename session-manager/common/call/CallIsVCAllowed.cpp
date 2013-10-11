/**
 * Class for rpc call IsVirtualChannelAllowed (freerds to session manager)
 *
 * Copyright 2013 Thinstuff Technologies GmbH
 * Copyright 2013 DI (FH) Martin Haimberger <martin.haimberger@thinstuff.at>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "CallIsVCAllowed.h"
#include <ICP.pb.h>

using freerds::icp::IsChannelAllowedRequest;
using freerds::icp::IsChannelAllowedResponse;

namespace freerds{
	namespace sessionmanager{
		namespace call{

		CallIsVCAllowed::CallIsVCAllowed() {

		};

		CallIsVCAllowed::~CallIsVCAllowed() {

		};

		unsigned long CallIsVCAllowed::getCallType() {
			return 1;
		};

		int CallIsVCAllowed::decodeRequest() {
			// decode protocol buffers
			IsChannelAllowedRequest req;
			if (!req.ParseFromString(mEncodedRequest)) {
				// failed to parse
				mResult = 1;// will report error with answer
				return -1;
			}
			mVirtualChannelName = req.channelname();
			return 0;
		};

		int CallIsVCAllowed::encodeResponse() {
			// encode protocol buffers
			IsChannelAllowedResponse resp;
			resp.set_channelallowed(mVirtualChannelAllowed);

			if (!resp.SerializeToString(&mEncodedResponse)) {
				// failed to serialize
				mResult = 1;
				return -1;
			}
			return 0;
		};

		int CallIsVCAllowed::doStuff() {
			// find out if Virtual Channel is allowed
			mVirtualChannelAllowed = true;
			return 0;
		}


		}
	}
}
