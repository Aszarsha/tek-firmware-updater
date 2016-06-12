#define COMPILE_AND_EXEC /*
printf "Compiling %s into %s\n" "${0}" "${0%.*}"
$CC -ggdb -Wall -pedantic -std=c11 -lusb-1.0 -o "${0%.*}" "${0}"
[ $? -ne 0 ] && exit
printf "Running %s" "${0%.*}\n"
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all "./${0%.*}" $*
exit
*/
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#include <libusb-1.0/libusb.h>

typedef uint8_t  ui8;
typedef uint16_t ui16;

#define TECK_VENDOR_ID                     0x0E6A
#define TECK_PRODUCT_ID_NORMAL_STATE       0x030C
#define TECK_PRODUCT_ID_PROGRAMMABLE_STATE 0x030C

enum tek_device_state_t {
		TEK_NORMAL_STATE       = TECK_PRODUCT_ID_NORMAL_STATE,
		TEK_PROGRAMMABLE_STATE = TECK_PRODUCT_ID_PROGRAMMABLE_STATE
};

// Megawin MG84FL54B doc says 16k of onboard ISP/IAP flash memory
// Otherwise, 8bit mode ihex files support addressing up to 65536
#define IHEX_BUFFER_MAX_SZ 16384

char const * load_ihex_buffer_from_file( char const * filename, ui8 * buffer, size_t * buffer_sz );

char const * get_handle_to_tek_device( libusb_device_handle* * usb_device_handle
                                     , enum tek_device_state_t * tek_device_state
                                     );
char const * upload_buffer_to_dev( ui8 * buffer, size_t buffer_sz, libusb_device_handle * usb_device_handle );

#define MAX_ERROR_STRING_SZ 256
static char const * format_error( char const * format, ... ) {
		static /*thread_local*/ char error_buffer[MAX_ERROR_STRING_SZ];
		va_list args;
		va_start( args, format );
		vsnprintf( error_buffer, MAX_ERROR_STRING_SZ, format, args );
		va_end( args );
		return error_buffer;
}

int main( int argc, char *argv[] ) {
		char const * opt_error = NULL;

		if ( argc != 2 ) {
				opt_error = format_error( "Usage: %s <firmware file>\n"
				                          "\tFile must be in Intel 8bit hex format"
				                        , argv[0]
				                        );
				goto program_exit;
		}

		printf( "Loading ihex firmware file.\n" );

		// First load the ihex file, if there is a problem
		// we want to exit early and not change the controller state
		size_t ihex_buffer_sz = IHEX_BUFFER_MAX_SZ;
		ui8 ihex_buffer[IHEX_BUFFER_MAX_SZ];
		char const * call_error = load_ihex_buffer_from_file( argv[1], ihex_buffer, &ihex_buffer_sz );
		if ( call_error ) {
				opt_error = format_error( "Unable to load hex buffer from file: %s", call_error );
				goto program_exit;
		}

		for ( size_t i = 0; i < ihex_buffer_sz; ++i ) {
				printf( "%x", (int)ihex_buffer[i] );
		}
		printf( "\n" );

		printf( "Searching for connected TEK\n" );

		int status = libusb_init( NULL );
		if ( status < 0 ) {
				opt_error = format_error( "Unable to initialize libusb: %s (%s)"
				                        , libusb_error_name( status ), libusb_strerror( status )
				                        );
				goto program_exit;
		}

		libusb_device_handle * tek_device_handle;
		enum tek_device_state_t tek_device_state = TEK_NORMAL_STATE ;
		call_error = get_handle_to_tek_device( &tek_device_handle, &tek_device_state );
		if ( call_error ) {
				opt_error = format_error( "Unable to connect to a TEK: %s", call_error );
				goto unwind_usb;
		}
		if ( tek_device_state != TEK_NORMAL_STATE ) {
				opt_error = "Found TEK, but is not in normal mode";
				goto unwind_tek_device_handle;
		}

		printf( "TEK found, switching to programmable mode.\n" );

		// TODO Actually switch

		printf( "Command sent, trying to reconnect.\n" );

		libusb_close( tek_device_handle );
		call_error = get_handle_to_tek_device( &tek_device_handle, &tek_device_state );
		if ( call_error ) {
				opt_error = format_error( "Unable to reconnect to the TEK: %s", call_error );
				goto unwind_usb;
		}
		if ( tek_device_state != TEK_PROGRAMMABLE_STATE ) {
				opt_error = "Found TEK, but is not in programmable mode";
				goto unwind_tek_device_handle;
		}

		printf( "TEK successfully switched to programmable mode.\n" );
		printf( "Sending new firmware to device.\n" );

		call_error = upload_buffer_to_dev( ihex_buffer, ihex_buffer_sz, tek_device_handle );
		if ( call_error ) {
				opt_error = format_error( "Unable to upload buffer to device: %s", call_error );
				goto unwind_tek_device_handle;
		}

		printf( "Firmware sent, switching back to normal mode.\n" );

		// TODO Actually switch

	unwind_tek_device_handle:
		libusb_close( tek_device_handle );
	unwind_usb:
		libusb_exit( NULL );
	program_exit:
		if ( opt_error ) {
				fprintf( stderr, "Error: %s\n", opt_error );
				return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
}

//=== Intel HEX format loading ===//

typedef struct {
		ui8    size;
		ui16   addr;
		ui8    type;
		ui8    data[256];
		ui8    checksum;
} ihex_record;

static char const * read_record_from_line( char const * line, ihex_record * record, ui8 * check_sum ) {
		assert( line && record && check_sum );

		size_t length = strlen( line );
		// Do not count carriage return and new line in the ihex line length
		while ( length > 0 && (line[length-1] == '\n' || line[length-1] == '\r') ) {   length -= 1;   }

		// check that line contains all records and start code BUT data
		// 11 characters total
		if ( length < 11 )    {   return "Invalid line length";   }
		if ( line[0] != ':' ) {   return "Invalid start code";    }

		int size, addr, type;
		int status = sscanf( line+1, "%2x%4x%2x", &size, &addr, &type );
		if ( status != 3 ) {   return "Error parsing record header";   }
		// check that line contains all records and start code AND data
		if ( length != 11+2*size ) {   return "Invalid line length";   }
		record->size = (ui8)size;    *check_sum  = (ui8)size;
		record->addr = (ui16)addr;   *check_sum += (ui8)(addr>>8);
		                             *check_sum += (ui8)addr;
		record->type = (ui8)type;    *check_sum += (ui8)type;

		for ( size_t i = 0; i < size; ++i ) {
				int datum;
				status = sscanf( line+9+2*i, "%2x", &datum );
				if ( status != 1 ) {   return "Error parsing record data";   }
				record->data[i] = (ui8)datum;   *check_sum += (ui8)datum;
		}
		int checksum;
		status = sscanf( line+9+2*size, "%2x", &checksum );
		if ( status != 1 ) {   return "Error parsing record chechsum";   }
		record->checksum = (ui8)checksum;

		return NULL;
}

// Data field contains 255 max bytes (since size field is 8bits, 0xFF)
// Line is 1 ':', 2 hex for size, 4 for addr, 2 for type, 255*2 (max) for data, and 2 for checksum
// plus two bytes for new line (/r/n) and one for null-termination
#define IHEX_LINE_MAX_SZ 521

static char const * load_ihex_buffer( FILE * ihex_file, ui8 * buffer, size_t * buffer_sz ) {
		assert( ihex_file && buffer && buffer_sz && *buffer_sz );

		size_t line_number = 0;
		size_t highest_addr = 0;
		bool last_record_read = false;
		while ( true ) {
				line_number += 1;

				char line_buffer[IHEX_LINE_MAX_SZ];
				if ( !fgets( line_buffer, IHEX_LINE_MAX_SZ, ihex_file ) ) {
						if ( feof( ihex_file ) ) {
								if ( last_record_read ) {   break;   } // we're fine! :)
								return format_error( "Unexpected end-of-file (line %zu)", line_number );
						}
						assert( ferror( ihex_file ) );
						return format_error( "Error reading ihex file (line %zu): %s", line_number, strerror( errno ) );
				}
				if ( last_record_read ) {
						return format_error( "Data after last record (line %zu)", line_number );
				}

				ihex_record record;
				ui8 check_sum;
				char const * error = read_record_from_line( line_buffer, &record, &check_sum );
				if ( error ) {   return error;   }

				if ( (ui8)(check_sum + record.checksum) != 0 ) {
						return format_error( "Invalid record (line %zu)", line_number );
				}

				switch ( record.type ) {
					case 0: {
							size_t end_addr = record.addr + record.size;
							if ( end_addr >= *buffer_sz ) {
									return format_error( "Addr too high to upload (line %zu)", line_number );
							}
							if ( end_addr > highest_addr ) {
									highest_addr = end_addr;
							}
							for ( size_t i = 0; i < record.size; ++i ) {
									buffer[record.addr+i] = record.data[i];
							}
						} break;
					case 1: {
							last_record_read = true;
						} break;
					case 2:
					case 3:
					case 4:
					case 5:
						return format_error( "Only support 8bit ihex format (line %zu)", line_number );
					default:
						return format_error( "Invalid record type (line %zu)", line_number );
				}
		}
		*buffer_sz = highest_addr;
		return NULL;
}

char const * load_ihex_buffer_from_file( char const * filename, ui8 * ihex_buffer, size_t * ihex_buffer_sz ) {
		char const * opt_error = NULL;

		FILE * ihex_file = fopen( filename, "r" );
		if ( !ihex_file ) {
				opt_error = format_error( "Unable to open ihex file \"%s\"", filename );
				goto function_exit;
		}

		char const * call_error = load_ihex_buffer( ihex_file, ihex_buffer, ihex_buffer_sz );
		if ( call_error ) {
				opt_error = format_error(  "Unable to load hex file: %s", call_error );
				goto unwind_file;
		}

	unwind_file:
		fclose( ihex_file );
	function_exit:
		return opt_error;
}

//=== USB Device Firmware Update upload ===//

char const * get_handle_to_tek_device( libusb_device_handle* * tek_device_handle
                                     , enum tek_device_state_t * tek_device_state
                                     ) {
		char const * opt_error = NULL;

		libusb_device* * usb_devices;
		int status = libusb_get_device_list( NULL, &usb_devices );
		if ( status < 0 ) {
				opt_error = format_error( "Unable to enumerate usb devices: %s (%s)"
				                        , libusb_strerror( status ), libusb_error_name( status )
				                        );
				goto function_exit;
		}

		libusb_device * tek_device = NULL;
		for ( libusb_device* * it = usb_devices; *it; ++it ) {
				struct libusb_device_descriptor usb_device_descriptor;
				status = libusb_get_device_descriptor( *it, &usb_device_descriptor );
				if ( status < 0 ) {
						opt_error = format_error( "Unable to usb get device descriptor: %s (%s)"
						                        , libusb_strerror( status ), libusb_error_name( status )
						                        );
						goto unwind_usb_devices_list;
				}

				if ( usb_device_descriptor.idVendor == TECK_VENDOR_ID ) {
						if ( usb_device_descriptor.idProduct == TECK_PRODUCT_ID_NORMAL_STATE
						  || usb_device_descriptor.idProduct == TECK_PRODUCT_ID_PROGRAMMABLE_STATE
						   ) {
								if ( tek_device ) {
										opt_error = "Multiple TEK keyboards found; make sure to connect only one";
										goto unwind_usb_devices_list;
								}
								tek_device = *it;
								*tek_device_state = usb_device_descriptor.idProduct;
						}
				}
		}

		if ( !tek_device ) {
				opt_error = "Unable to find a TEK keyboard device connected";
		} else {
				status = libusb_open( tek_device, tek_device_handle );
				if ( status < 0 ) {
						opt_error = format_error( "Unable to get a handle on the TEK device: %s (%s)"
						                        , libusb_strerror( status ), libusb_error_name( status )
						                        );
				}
		}

	unwind_usb_devices_list:
		libusb_free_device_list( usb_devices, true );
	function_exit:
		return opt_error;
}

char const * upload_buffer_to_dev( ui8 * buffer, size_t buffer_sz, libusb_device_handle * usb_device_handle ) {
		// TODO Actually send the buffer

		return NULL;
}
