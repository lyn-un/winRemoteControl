#ifndef _WINREMOTECONTROL_PRIVACY_PRIVACYMODESERVICE_H_
#define _WINREMOTECONTROL_PRIVACY_PRIVACYMODESERVICE_H_

#include "core/privacy/displaypoweradapter.h"
#include "core/privacy/privacyoverlayadapter.h"

#include <QtCore/QObject>
#include <QtCore/QStringList>

#include <memory>

class KPrivacyModeService : public QObject
{
	Q_OBJECT

public:
	explicit KPrivacyModeService(
		std::unique_ptr<IKPrivacyOverlayAdapter> spOverlayAdapter,
		std::unique_ptr<IKDisplayPowerAdapter> spDisplayPowerAdapter,
		QObject *pParent = nullptr);
	~KPrivacyModeService() override;

	KPrivacyModeService(const KPrivacyModeService &) = delete;
	KPrivacyModeService &operator=(const KPrivacyModeService &) = delete;

	void beginSession(quint64 nGeneration);
	QStringList supportedModes(bool bAdvertiseDisplayOff) const;
	KPrivacyOperationResult setMode(KPrivacyMode mode,
		const QString &strRequestId,
		quint64 nGeneration);
	KPrivacyOperationResult reset(quint64 nGeneration);
	KPrivacyModeStatus status() const;

signals:
	void statusChanged(const KPrivacyModeStatus &status);
	void emergencyRestoreTriggered(quint64 nGeneration);

private:
	KPrivacyOperationResult applyMode(KPrivacyMode mode);
	KPrivacyOperationResult restoreMode(KPrivacyMode mode);
	void publishStatus();
	void handleEmergencyRestore();

	std::unique_ptr<IKPrivacyOverlayAdapter> m_spOverlayAdapter;
	std::unique_ptr<IKDisplayPowerAdapter> m_spDisplayPowerAdapter;
	KPrivacyModeStatus m_status;
};

#endif // _WINREMOTECONTROL_PRIVACY_PRIVACYMODESERVICE_H_
