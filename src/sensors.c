/**
 * collectd - src/sensors.c
 * Copyright (C) 2005,2006  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 *   
 *   Lubos Stanek <lubek at users.sourceforge.net> Wed Oct 25, 2006
 *   - config ExtendedSensorNaming option
 *   - precise sensor feature selection (chip-bus-address/type-feature)
 *     with ExtendedSensorNaming
 *   - more sensor features (finite list)
 *   - honor sensors.conf's ignored
 *   - config Sensor option
 *   - config IgnoreSelected option
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"
#include "utils_debug.h"

#define MODULE_NAME "sensors"

#if defined(HAVE_SENSORS_SENSORS_H)
# include <sensors/sensors.h>
#else
# undef HAVE_LIBSENSORS
#endif

#if defined(HAVE_LIBSENSORS)
# define SENSORS_HAVE_READ 1
#else
# define SENSORS_HAVE_READ 0
#endif

#define BUFSIZE 512

static char *ds_def[] =
{
	"DS:value:GAUGE:"COLLECTD_HEARTBEAT":U:U",
	NULL
};
static int ds_num = 1;

/* old naming */
static char *filename_format = "sensors-%s.rrd";
/* end old naming */

/* new naming */
static char *sensor_filename_format = "lm_sensors-%s.rrd";

static char *sensor_voltage_ds_def[] = 
{
	"DS:voltage:GAUGE:"COLLECTD_HEARTBEAT":U:U",
	NULL
};
static int sensor_voltage_ds_num = 1;

#define SENSOR_TYPE_UNKNOWN 0
#define SENSOR_TYPE_VOLTAGE 1
#define SENSOR_TYPE_FANSPEED 2
#define SENSOR_TYPE_TEMPERATURE 3

static char *sensor_type_prefix[] =
{
    "/unknown",
    "/voltage",
    "/fanspeed",
    "/temperature",
    NULL
};

typedef struct sensors_labeltypes {
    char *label;
    int type;
} sensors_labeltypes;

/* finite list of known labels
 * sorted reverse by the length for the same type
 * because strncmp must match "temp1" before "temp"
 */
static sensors_labeltypes known_features[] = 
{
    { "fan7", SENSOR_TYPE_FANSPEED },
    { "fan6", SENSOR_TYPE_FANSPEED },
    { "fan5", SENSOR_TYPE_FANSPEED },
    { "fan4", SENSOR_TYPE_FANSPEED },
    { "fan3", SENSOR_TYPE_FANSPEED },
    { "fan2", SENSOR_TYPE_FANSPEED },
    { "fan1", SENSOR_TYPE_FANSPEED },
    { "in8", SENSOR_TYPE_VOLTAGE },
    { "in7", SENSOR_TYPE_VOLTAGE },
    { "in6", SENSOR_TYPE_VOLTAGE },
    { "in5", SENSOR_TYPE_VOLTAGE },
    { "in4", SENSOR_TYPE_VOLTAGE },
    { "in3", SENSOR_TYPE_VOLTAGE },
    { "in2", SENSOR_TYPE_VOLTAGE },
    { "in0", SENSOR_TYPE_VOLTAGE },
    { "remote_temp", SENSOR_TYPE_TEMPERATURE },
    { "temp7", SENSOR_TYPE_TEMPERATURE },
    { "temp6", SENSOR_TYPE_TEMPERATURE },
    { "temp5", SENSOR_TYPE_TEMPERATURE },
    { "temp4", SENSOR_TYPE_TEMPERATURE },
    { "temp3", SENSOR_TYPE_TEMPERATURE },
    { "temp2", SENSOR_TYPE_TEMPERATURE },
    { "temp1", SENSOR_TYPE_TEMPERATURE },
    { "temp", SENSOR_TYPE_TEMPERATURE },
    { "Vccp2", SENSOR_TYPE_VOLTAGE },
    { "Vccp1", SENSOR_TYPE_VOLTAGE },
    { "vdd", SENSOR_TYPE_VOLTAGE },
    { "vid4", SENSOR_TYPE_VOLTAGE },
    { "vid3", SENSOR_TYPE_VOLTAGE },
    { "vid2", SENSOR_TYPE_VOLTAGE },
    { "vid1", SENSOR_TYPE_VOLTAGE },
    { "vid", SENSOR_TYPE_VOLTAGE },
    { "vin4", SENSOR_TYPE_VOLTAGE },
    { "vin3", SENSOR_TYPE_VOLTAGE },
    { "vin2", SENSOR_TYPE_VOLTAGE },
    { "vin1", SENSOR_TYPE_VOLTAGE },
    { "voltbatt", SENSOR_TYPE_VOLTAGE },
    { "volt12", SENSOR_TYPE_VOLTAGE },
    { "volt5", SENSOR_TYPE_VOLTAGE },
    { "vrm", SENSOR_TYPE_VOLTAGE },
    { "12V", SENSOR_TYPE_VOLTAGE },
    { "2.5V", SENSOR_TYPE_VOLTAGE },
    { "3.3V", SENSOR_TYPE_VOLTAGE },
    { "5V", SENSOR_TYPE_VOLTAGE },
    { 0, -1 }
};
/* end new naming */

static char *config_keys[] =
{
	"Sensor",
	"IgnoreSelected",
	"ExtendedSensorNaming",
	NULL
};
static int config_keys_num = 3;

static char **sensor_list = NULL;
static int sensor_list_num = 0;
/* 
 * sensor_list_action:
 * 0 => default is to collect selected sensors
 * 1 => ignore selected sensors
 */
static int sensor_list_action = 0;
/* 
 * sensor_extended_naming:
 * 0 => default is to create chip-feature
 * 1 => use new naming scheme chip-bus-address/type-feature
 */
static int sensor_extended_naming = 0;

#ifdef HAVE_LIBSENSORS
typedef struct featurelist
{
	const sensors_chip_name    *chip;
	const sensors_feature_data *data;
	int		    	    type;
	struct featurelist         *next;
} featurelist_t;

featurelist_t *first_feature = NULL;
#endif /* defined (HAVE_LIBSENSORS) */

static int sensors_config (char *key, char *value)
{
	char **temp;

	if (strcasecmp (key, "Sensor") == 0)
	{
		temp = (char **) realloc (sensor_list, (sensor_list_num + 1) * sizeof (char *));
		if (temp == NULL)
		{
			syslog (LOG_EMERG, "Cannot allocate more memory.");
			return (1);
		}
		sensor_list = temp;

		if ((sensor_list[sensor_list_num] = strdup (value)) == NULL)
		{
			syslog (LOG_EMERG, "Cannot allocate memory.");
			return (1);
		}
		sensor_list_num++;
	}
	else if (strcasecmp (key, "IgnoreSelected") == 0)
	{
		if ((strcasecmp (value, "True") == 0)
				|| (strcasecmp (value, "Yes") == 0)
				|| (strcasecmp (value, "On") == 0))
			sensor_list_action = 1;
		else
			sensor_list_action = 0;
	}
	else if (strcasecmp (key, "ExtendedSensorNaming") == 0)
	{
		if ((strcasecmp (value, "True") == 0)
				|| (strcasecmp (value, "Yes") == 0)
				|| (strcasecmp (value, "On") == 0))
			sensor_extended_naming = 1;
		else
			sensor_extended_naming = 0;
	}
	else
	{
		return (-1);
	}

	return (0);
}

/*
 * Check if this feature should be ignored. This is called from
 * both, `submit' and `write' to give client and server
 *  the ability to ignore certain stuff...
 */
static int config_get_ignored (const char *inst)
{
	int i;

	/* If no ignored are given collect all features. */
	if (sensor_list_num < 1)
		return (0);

	for (i = 0; i < sensor_list_num; i++)
		if (strcasecmp (inst, sensor_list[i]) == 0)
			return (sensor_list_action);
	return (1 - sensor_list_action);
}

static void collectd_sensors_init (void)
{
#ifdef HAVE_LIBSENSORS
	FILE *fh;
	featurelist_t *last_feature = NULL;
	featurelist_t *new_feature;
	
	const sensors_chip_name *chip;
	int chip_num;

	const sensors_feature_data *data;
	int data_num0, data_num1;
	
	new_feature = first_feature;
	while (new_feature != NULL)
	{
		last_feature = new_feature->next;
		free (new_feature);
		new_feature = last_feature;
	}

#ifdef assert
	assert (new_feature == NULL);
	assert (last_feature == NULL);
#endif

	if ((fh = fopen ("/etc/sensors.conf", "r")) == NULL)
		return;

	if (sensors_init (fh))
	{
		fclose (fh);
		syslog (LOG_ERR, "sensors: Cannot initialize sensors. Data will not be collected.");
		return;
	}

	fclose (fh);

	chip_num = 0;
	while ((chip = sensors_get_detected_chips (&chip_num)) != NULL)
	{
		data = NULL;
		data_num0 = data_num1 = 0;

		while ((data = sensors_get_all_features (*chip, &data_num0, &data_num1)) != NULL)
		{
			/* "master features" only */
			if (data->mapping != SENSORS_NO_MAPPING)
				continue;

			/* Only known features */
			int i = 0;
			while (known_features[i].type >= 0)
			{
				if(strncmp(data->name, known_features[i].label, strlen(known_features[i].label)) == 0)
				{
					/* skip ignored in sensors.conf */
					if (sensors_get_ignored(*chip, data->number) == 0)
					{
					    break;
					}

					if ((new_feature = (featurelist_t *) malloc (sizeof (featurelist_t))) == NULL)
					{
						perror ("malloc");
						break;
					}

					DBG ("Adding feature: %s/%s/%i", chip->prefix, data->name, known_features[i].type);
					new_feature->chip = chip;
					new_feature->data = data;
					new_feature->type = known_features[i].type;
					new_feature->next = NULL;

					if (first_feature == NULL)
					{
						first_feature = new_feature;
						last_feature  = new_feature;
					}
					else
					{
						last_feature->next = new_feature;
						last_feature = new_feature;
					}

					/* stop searching known features at first found */
					break;
				}
				i++;
			}
		}
	}

	if (first_feature == NULL)
		sensors_cleanup ();
#endif /* defined(HAVE_LIBSENSORS) */

	return;
}

static void sensors_write (char *host, char *inst, char *val)
{
	char file[BUFSIZE];
	int status;
	char *typestart;

	/* skip ignored in our config */
	if (config_get_ignored (inst))
	    return;

	/* extended sensor naming */
	if(sensor_extended_naming)
	    status = snprintf (file, BUFSIZE, sensor_filename_format, inst);
	else
	    status = snprintf (file, BUFSIZE, filename_format, inst);
	if (status < 1)
		return;
	else if (status >= BUFSIZE)
		return;

	if(sensor_extended_naming)
	{
	    typestart = strrchr(inst, '/');
	    if(typestart != NULL)
	    {
		if(strncmp(typestart, sensor_type_prefix[SENSOR_TYPE_VOLTAGE], strlen(sensor_type_prefix[SENSOR_TYPE_VOLTAGE])) == 0)
		    rrd_update_file (host, file, val, sensor_voltage_ds_def, sensor_voltage_ds_num);
		else
		    rrd_update_file (host, file, val, ds_def, ds_num);
	    }
	    else
		return;
	}
	else
	    rrd_update_file (host, file, val, ds_def, ds_num);
}

#if SENSORS_HAVE_READ
static void sensors_submit (const char *feat_name, const char *chip_prefix, double value)
{
	char buf[BUFSIZE];
	char inst[BUFSIZE];

	if (snprintf (inst, BUFSIZE, "%s-%s", chip_prefix, feat_name) >= BUFSIZE)
		return;

	/* skip ignored in our config */
	if (config_get_ignored (inst))
	    return;

	if (snprintf (buf, BUFSIZE, "%u:%.3f", (unsigned int) curtime, value) >= BUFSIZE)
		return;

	DBG ("%s, %s", inst, buf);
	plugin_submit (MODULE_NAME, inst, buf);
}

static void sensors_read (void)
{
	featurelist_t *feature;
	double value;
	char chip_fullprefix[BUFSIZE];

	for (feature = first_feature; feature != NULL; feature = feature->next)
	{
	    if (sensors_get_feature (*feature->chip, feature->data->number, &value) < 0)
		continue;

	    if(sensor_extended_naming)
	    {
		/* full chip name logic borrowed from lm_sensors */
		if (feature->chip->bus == SENSORS_CHIP_NAME_BUS_ISA)
		{
		    if (snprintf (chip_fullprefix, BUFSIZE, "%s-isa-%04x%s", feature->chip->prefix, feature->chip->addr, sensor_type_prefix[feature->type]) >= BUFSIZE)
			continue;
		}
		else if (feature->chip->bus == SENSORS_CHIP_NAME_BUS_DUMMY)
		{
		    if (snprintf (chip_fullprefix, BUFSIZE, "%s-%s-%04x%s", feature->chip->prefix, feature->chip->busname, feature->chip->addr, sensor_type_prefix[feature->type]) >= BUFSIZE)
			continue;
		}
		else
		{
		    if (snprintf (chip_fullprefix, BUFSIZE, "%s-i2c-%d-%02x%s", feature->chip->prefix, feature->chip->bus, feature->chip->addr, sensor_type_prefix[feature->type]) >= BUFSIZE)
			continue;
		}
		sensors_submit (feature->data->name, (const char *)chip_fullprefix, value);
	    }
	    else
	    {
		sensors_submit (feature->data->name, feature->chip->prefix, value);
	    }
	}
}
#else
# define sensors_read NULL
#endif /* SENSORS_HAVE_READ */

void module_register (void)
{
	plugin_register (MODULE_NAME, collectd_sensors_init, sensors_read, sensors_write);
	cf_register (MODULE_NAME, sensors_config, config_keys, config_keys_num);
}

#undef BUFSIZE
#undef MODULE_NAME
