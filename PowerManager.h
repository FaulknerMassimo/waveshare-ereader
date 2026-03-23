#pragma once

#include <Arduino.h>
#include <Wire.h>

#define AXP2101_I2C_ADDR  0x34

#define AXP2101_REG_STATUS1 0x00
#define AXP2101_REG_STATUS2 0x01
#define AXP2101_REG_VBAT_H 0x34
#define AXP2101_REG_VBAT_L 0x35
#define AXP2101_REG_VBAT_PERCENT 0xA4
#define AXP2101_REG_PMU_CFG 0x10
#define AXP2101_REG_PWR_OFF 0x10

class PowerManager
{
public:
	PowerManager(TwoWire &wire, int irq_pin)
		: m_wire(wire), m_irq_pin(irq_pin), m_battery_percent(50) {}

	bool begin()
	{
		if (m_irq_pin >= 0)
			pinMode(m_irq_pin, INPUT);

		m_wire.beginTransmission(AXP2101_I2C_ADDR);
		uint8_t err = m_wire.endTransmission();
		if (err != 0)
		{
			Serial.printf("[AXP2101] Not found on I2C (err=%d)\n", err);
			return false;
		}

		Serial.println("[AXP2101] Found");

		write_reg(0xA2, read_reg(0xA2) | 0x02);

		refresh();
		return true;
	}

	void refresh()
	{
		uint8_t val = read_reg(AXP2101_REG_VBAT_PERCENT);
		m_battery_percent = std::min((int)(val & 0x7F), 100);
	}

	int battery_percent() const { return m_battery_percent; }
	int battery_voltage_mv()
	{
		uint8_t hi = read_reg(AXP2101_REG_VBAT_H);
		uint8_t lo = read_reg(AXP2101_REG_VBAT_L);
		// 14-bit value, LSB = 1.1 mV
		uint16_t raw = ((uint16_t)(hi & 0x3F) << 8) | lo;
		return (int)(raw * 1.1f);
	}

	void power_off()
	{
		write_reg(0x10, read_reg(0x10) | 0x01);
	}

private:
	TwoWire &m_wire;
	int      m_irq_pin;
	int      m_battery_percent;

	uint8_t read_reg(uint8_t reg)
	{
		m_wire.beginTransmission(AXP2101_I2C_ADDR);
		m_wire.write(reg);
		if (m_wire.endTransmission(false) != 0) return 0xFF;
		m_wire.requestFrom((uint8_t)AXP2101_I2C_ADDR, (uint8_t)1);
		if (m_wire.available()) return m_wire.read();
		return 0xFF;
	}

	void write_reg(uint8_t reg, uint8_t val)
	{
		m_wire.beginTransmission(AXP2101_I2C_ADDR);
		m_wire.write(reg);
		m_wire.write(val);
		m_wire.endTransmission();
	}
};
