#include "automation/driver/base/commandcontext.h"

bool KDriverCommandContext::complete(const QJsonObject &response)
{
	if (bCompleted || bTimedOut)
		return false;
	bCompleted = true;
	result = response;
	if (completed)
		completed(result);
	return true;
}

bool KDriverCommandContext::timeout(const QJsonObject &response)
{
	if (bCompleted || bTimedOut)
		return false;
	bTimedOut = true;
	result = response;
	if (completed)
		completed(result);
	return true;
}
