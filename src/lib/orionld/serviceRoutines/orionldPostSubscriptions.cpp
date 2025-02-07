/*
*
* Copyright 2018 FIWARE Foundation e.V.
*
* This file is part of Orion-LD Context Broker.
*
* Orion-LD Context Broker is free software: you can redistribute it and/or
* modify it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* Orion-LD Context Broker is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero
* General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with Orion-LD Context Broker. If not, see http://www.gnu.org/licenses/.
*
* For those usages not covered by this license please contact with
* orionld at fiware dot org
*
* Author: Ken Zangelin
*/
#include <string>
#include <vector>

#include "logMsg/logMsg.h"                                     // LM_*
#include "logMsg/traceLevels.h"                                // Lmt*

extern "C"
{
#include "kalloc/kaStrdup.h"                                   // kaStrdup
#include "kjson/kjLookup.h"                                    // kjLookup
}

#include "common/globals.h"                                    // parse8601Time
#include "rest/OrionError.h"                                   // OrionError
#include "rest/ConnectionInfo.h"                               // ConnectionInfo
#include "rest/httpHeaderAdd.h"                                // httpHeaderLocationAdd
#include "apiTypesV2/HttpInfo.h"                               // HttpInfo
#include "apiTypesV2/Subscription.h"                           // Subscription
#include "mongoBackend/mongoGetSubscriptions.h"                // mongoGetLdSubscription
#include "mongoBackend/mongoCreateSubscription.h"              // mongoCreateSubscription
#include "orionld/common/orionldState.h"                       // orionldState
#include "orionld/common/orionldErrorResponse.h"               // orionldErrorResponseCreate
#include "orionld/context/orionldCoreContext.h"                // ORIONLD_CORE_CONTEXT_URL
#include "orionld/kjTree/kjTreeToEntIdVector.h"                // kjTreeToEntIdVector
#include "orionld/kjTree/kjTreeToStringList.h"                 // kjTreeToStringList
#include "orionld/kjTree/kjTreeToSubscriptionExpression.h"     // kjTreeToSubscriptionExpression
#include "orionld/kjTree/kjTreeToEndpoint.h"                   // kjTreeToEndpoint
#include "orionld/kjTree/kjTreeToNotification.h"               // kjTreeToNotification
#include "orionld/kjTree/kjTreeToSubscription.h"               // kjTreeToSubscription
#include "orionld/mqtt/mqttParse.h"                            // mqttParse
#include "orionld/mqtt/mqttConnectionEstablish.h"              // mqttConnectionEstablish
#include "orionld/serviceRoutines/orionldPostSubscriptions.h"  // Own Interface



// ----------------------------------------------------------------------------
//
// orionldPostSubscriptions -
//
// A ngsi-ld subscription contains the following fields:
// - id                 Subscription::id                                              (URI given by creating request or auto-generated)
// - type               Not in DB                                                     (must be "Subscription" - will not be saved in mongo)
// - name               NOT SUPPORTED                                                 (String)
// - description        Subscription::description                                     (String)
// - entities           Subscription::Subject::entities                               (Array of EntityInfo which is a subset of EntID)
// - watchedAttributes  Subscription::Notification::attributes                        (Array of String)
// - timeInterval       NOT SUPPORTED                                                 (will not be implemented any time soon - not very useful)
// - q                  Subscription::Subject::Condition::SubscriptionExpression::q
// - geoQ               NOT SUPPORTED
// - csf                NOT SUPPORTED
// - isActive           May not be necessary to store in mongo - "status" is enough?
// - notification       Subscription::Notification + Subscription::attrsFormat?
// - expires            Subscription::expires                                         (DateTime)
// - throttling         Subscription::throttling                                      (Number - in seconds)
// - status             Subscription::status                                          (builtin String: "active", "paused", "expired")
//
// * At least one of 'entities' and 'watchedAttributes' must be present.
// * Either 'timeInterval' or 'watchedAttributes' must be present. But not both of them
// * For now, 'timeInterval' will not be implemented. If ever ...
//
bool orionldPostSubscriptions(ConnectionInfo* ciP)
{
  ngsiv2::Subscription sub;
  std::string          subId;
  OrionError           oError;

  if (orionldState.contextP != NULL)
    sub.ldContext = orionldState.contextP->url;
  else
    sub.ldContext = ORIONLD_CORE_CONTEXT_URL;

  //
  // FIXME: attrsFormat etc. should be set to default by constructor
  //        only ... there is no constructor ...
  //
  sub.attrsFormat = DEFAULT_RENDER_FORMAT;
  sub.expires             = -1;  // 0?
  sub.throttling          = -1;  // 0?
  sub.timeInterval        = -1;  // 0?

  LM_T(LmtServiceRoutine, ("In orionldPostSubscriptions - calling kjTreeToSubscription"));
  char*    subIdP    = NULL;
  KjNode*  endpointP = NULL;

  if (kjTreeToSubscription(&sub, &subIdP, &endpointP) == false)
  {
    LM_E(("kjTreeToSubscription FAILED"));
    // orionldErrorResponseCreate is invoked by kjTreeToSubscription
    return false;
  }

  if (endpointP != NULL)
  {
    KjNode* uriP = kjLookup(endpointP, "uri");

    if (strncmp(uriP->value.s, "mqtt://", 7) == 0)
    {
      bool            mqtts         = false;
      char*           mqttUser      = NULL;
      char*           mqttPassword  = NULL;
      char*           mqttHost      = NULL;
      unsigned short  mqttPort      = 0;
      char*           mqttTopic     = NULL;
      char*           detail        = NULL;
      char*           uri           = kaStrdup(&orionldState.kalloc, uriP->value.s);  // Can't destroy uriP->value.s ... mqttParse is destructive!

      if (mqttParse(uri, &mqtts, &mqttUser, &mqttPassword, &mqttHost, &mqttPort, &mqttTopic, &detail) == false)
      {
        LM_W(("Bad Input (invalid MQTT endpoint)"));
        orionldErrorResponseCreate(OrionldBadRequestData, "Invalid MQTT endpoint", detail);
        orionldState.httpStatusCode = SccBadRequest;
        return false;
      }

      if (mqttConnectionEstablish(mqtts, mqttUser, mqttPassword, mqttHost, mqttPort) == false)
      {
        LM_E(("Internal Error (unable to connect to MQTT server)"));
        orionldErrorResponseCreate(OrionldInternalError, "Unable to connect to MQTT server", "xxx");
        orionldState.httpStatusCode = SccReceiverInternalError;
        return false;
      }
    }
  }

  //
  // Does the subscription already exist?
  //
  // FIXME: Implement a function to ONLY check for existence - much faster
  //
  if (subIdP != NULL)
  {
    ngsiv2::Subscription  subscription;
    char*                 details;

    // mongoGetLdSubscription takes the req semaphore
    if (mongoGetLdSubscription(&subscription, subIdP, orionldState.tenant, &orionldState.httpStatusCode, &details) == true)
    {
      orionldErrorResponseCreate(OrionldBadRequestData, "A subscription with that ID already exists", subIdP);
      orionldState.httpStatusCode = SccConflict;
      return false;
    }
  }

  //
  // Create the subscription
  //
  subId = mongoCreateSubscription(sub,
                                  &oError,
                                  orionldState.tenant,
                                  ciP->servicePathV,
                                  ciP->httpHeaders.xauthToken,
                                  ciP->httpHeaders.correlator,
                                  sub.ldContext);

  // FIXME: Check oError for failure!
  orionldState.httpStatusCode = SccCreated;
  httpHeaderLocationAdd(ciP, "/ngsi-ld/v1/subscriptions/", subId.c_str());

  return true;
}
