/** @file   i2c_master.c
    @author M. P. Hayes, UCECE
    @date   12 April 2013
    @brief  Bit-bashed I2C master (TWI)

    This is written on a whim and is completely untested.

    Two PIOs are required for SCL and SDA.  Ideally, they should have
    schmitt trigger inputs to handle the slow rising edges of an
    open-drain bus.
*/

#include "i2c_master.h"
#include "i2c_private.h"
#include "delay.h"


#ifndef I2C_DEVICES_NUM
#define I2C_DEVICES_NUM 4
#endif



static uint8_t i2c_devices_num = 0;
static i2c_dev_t i2c_devices[I2C_DEVICES_NUM];



/**
   Preconditions: SCL output low, SDA don't care.
   Postconditions: SCL output low, SDA input
*/
static i2c_ret_t
i2c_master_recv_bit (i2c_t dev)
{
    i2c_ret_t ret;
    bool bit;

    i2c_sda_set (dev, 1);
    DELAY_US (4);    

    i2c_scl_set (dev, 1);

    ret = i2c_scl_wait (dev);
    if (ret != I2C_OK)
        return ret;

    bit = i2c_sda_get (dev);

    DELAY_US (4);    

    i2c_scl_set (dev, 0);
    
    return bit;
}



static i2c_ret_t
i2c_master_send_bit (i2c_t dev, bool bit)
{
    i2c_ret_t ret;

    /* The scl line should be low at this point.  The sda line can
     only be changed when scl is low.  */

    i2c_sda_set (dev, bit);

    i2c_scl_set (dev, 1);

    /* The receiver samples on the rising edge of scl but this is
       a slow transition.  Wait until the scl goes high to ensure
       that the receiver sees the bit.  The slave can also force
       scl low to stretch the clock and give it time to do
       something.  */
    ret = i2c_scl_wait (dev);
    if (ret != I2C_OK)
        return ret;

    /* Check if lost arbitration.  */
    if (bit && !i2c_sda_get (dev))
        return 0;
    
    DELAY_US (4);
    
    i2c_scl_set (dev, 0);
    return 1;
}


static i2c_ret_t
i2c_master_recv_ack (i2c_t dev)
{
    return i2c_master_recv_bit (dev);
}


static i2c_ret_t
i2c_master_send_ack (i2c_t dev)
{
    return i2c_master_send_bit (dev, 0);
}


static i2c_ret_t
i2c_master_send_nack (i2c_t dev)
{
    return i2c_master_send_bit (dev, 1);
}


/**
   Preconditions: SCL output low, SDA output indeterminate.
   Postconditions: SCL output low, SDA input
*/
static i2c_ret_t 
i2c_master_send_byte (i2c_t dev, uint8_t data)
{
    int i;
    i2c_ret_t ret;

    for (i = 0; i < 8; i++)
    {
        ret = i2c_master_send_bit (dev, (data >> 7) & 1);
        if (ret != I2C_OK)
            return ret;
        data <<= 1;
    }

    return i2c_master_recv_ack (dev);
}


static i2c_ret_t
i2c_master_recv_byte (i2c_t dev, uint8_t *data)
{
    int i;
    i2c_ret_t ret;
    uint8_t d = 0;

    for (i = 0; i < 8; i++)
    {
        ret = i2c_master_recv_bit (dev);
        if (ret != I2C_OK)
            return ret;

        d = (d << 1) | ret;
    }

    *data = d;

    return i2c_master_send_ack (dev);
}


static i2c_ret_t
i2c_master_send_start (i2c_t dev)
{
    /* The scl and sda lines should be high inputs at this point.  If
       not, some other master got in first.  */
    if (!i2c_sda_get (dev))
        return I2C_ERROR_CONFLICT;

    i2c_sda_set (dev, 0);
    DELAY_US (4);

    i2c_scl_set (dev, 0);
    return I2C_OK;
}


static i2c_ret_t
i2c_master_send_stop (i2c_t dev)
{
    i2c_sda_set (dev, 0);
    DELAY_US (4);

    i2c_scl_set (dev, 1);
    i2c_scl_wait (dev);
    DELAY_US (4);

    i2c_sda_set (dev, 1);
    /* It is possible to lose arbitration at this point but who cares? 
       We think we have finished!  */
    return I2C_OK;
}


static i2c_ret_t
i2c_master_send_addr (i2c_t dev, bool read)
{
    /* Send 7-bit slave address followed by bit to indicate
       read/write.  

       For 10-bit slave addresses, the second byte is part of the
       data packet.  */
    return i2c_master_send_byte (dev, (dev->slave->id << 1) | (read != 0));
}


int
i2c_master_transfer (i2c_t dev, void *buffer, uint8_t size, i2c_action_t action)
{
    uint8_t i;
    uint8_t *data = buffer;
    i2c_ret_t ret;

    if (action & I2C_START)
    {
        ret = i2c_master_send_start (dev);
        if (ret != I2C_OK)
            return ret;

        ret = i2c_master_send_addr (dev, action & I2C_READ);
        if (ret != I2C_OK)
            return ret;
    }

    /* Send or receive data packets.  */
    for (i = 0; i < size; i++)
    {
        if (action & I2C_WRITE)
            ret = i2c_master_send_byte (dev, data[i]);
        else
            ret = i2c_master_recv_byte (dev, &data[i]);

        if (ret != I2C_OK)
            return ret;
    }

    if (action & I2C_STOP)
    {
        ret = i2c_master_send_stop (dev);
        if (ret != I2C_OK)
            return ret;
    }

    return i;
}


i2c_ret_t
i2c_master_addr_read (i2c_t dev, i2c_addr_t addr, void *buffer, uint8_t size)
{
    i2c_ret_t ret;

    ret = i2c_master_transfer (dev, &addr, dev->slave->addr_bytes, 
                               I2C_START | I2C_WRITE);
    if (ret < 0)
        return ret;

    return i2c_master_transfer (dev, buffer, size, I2C_WRITE | I2C_STOP);
}


i2c_ret_t
i2c_master_addr_write (i2c_t dev, i2c_addr_t addr, void *buffer, uint8_t size)
{
    i2c_ret_t ret;

    ret = i2c_master_transfer (dev, &addr, dev->slave->addr_bytes, 
                               I2C_START | I2C_WRITE);
    if (ret < 0)
        return ret;

    return i2c_master_transfer (dev, buffer, size, I2C_WRITE | I2C_STOP);
}


i2c_t
i2c_master_init (const i2c_bus_cfg_t *bus_cfg, const i2c_slave_cfg_t *slave_cfg)
{
    i2c_dev_t *dev;

    if (i2c_devices_num >= I2C_DEVICES_NUM)
        return 0;

    dev = i2c_devices + i2c_devices_num;
    i2c_devices_num++;    

    dev->bus = bus_cfg;
    dev->slave = slave_cfg;
    
    i2c_sda_set (dev, 1);
    i2c_scl_set (dev, 1);

    return dev;
}