#ifndef TREADMILL_H
#define TREADMILL_H
#include <QObject>
#include "bluetoothdevice.h"

class treadmill:public bluetoothdevice
{
    Q_OBJECT

public:
    treadmill();
    void update_metrics(const bool watt_calc, const double watts);
    virtual metric currentInclination();
    virtual uint8_t fanSpeed();
    virtual bool connected();
    uint16_t watts(double weight);
    bluetoothdevice::BLUETOOTH_TYPE deviceType();
    void clearStats();
    void setLap();
    void setPaused(bool p);

public slots:
    virtual bool changeFanSpeed(uint8_t speed);
    virtual void changeSpeed(double speed);
    virtual void changeInclination(double inclination);
    virtual void changeSpeedAndInclination(double speed, double inclination);

signals:
    void tapeStarted();

protected:    
    metric Inclination;
    double requestSpeed = -1;
    double requestInclination = -1;
    double requestFanSpeed = -1;
};

#endif // TREADMILL_H
