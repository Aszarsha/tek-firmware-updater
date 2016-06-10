#define COMPILE_AND_EXEC /*
printf "Compiling %s into %s\n" "${0}" "${0%.*}"
$CC -Wall -pedantic -std=c11 -o "${0%.*}" "${0}"
[ $? -ne 0 ] && exit
printf "Running %s" "${0%.*}\n"
valgrind --tool=memcheck --leak-check=full "./${0%.*}" $*
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

typedef uint8_t  ui8;
typedef uint16_t ui16;

char * load_ihex_buffer( FILE * ihex_file, ui8 * buffer, size_t * buffer_sz );

char * upload_buffer_to_dev( ui8 * buffer, size_t buffer_sz );

// Megawin MG84FL54B doc says 16k of onboard ISP/IAP flash memory
// Otherwise, 8bit mode ihex files support addressing up to 65536
#define IHEX_BUFFER_MAX_SZ 16384

int main( int argc, char *argv[] ) {
		int exit_code = EXIT_FAILURE;
		if ( argc != 2 ) {
				printf( "Usage: %s <firmware file>\n\tFile must be in Intel 8bit hex format\n", argv[0] );
				goto exit_program;
		}

		FILE * ihex_file = fopen( argv[1], "r" );
		if ( !ihex_file ) {
				fprintf( stderr
				       , "Unable to open ihex file \"%s\"\n", argv[1]
				       );
				goto exit_program;
		}

		// First load the ihex file, if there is a problem
		// we want to exit early and not change the controller state
		size_t ihex_buffer_sz = IHEX_BUFFER_MAX_SZ;
		ui8 ihex_buffer[IHEX_BUFFER_MAX_SZ] = {0};
		char * load_error
			= load_ihex_buffer( ihex_file, ihex_buffer, &ihex_buffer_sz );
		if ( load_error ) {
				fprintf( stderr
				       , "Unable to load hex file: %s\n", load_error
				       );
				goto unwind_file;
		}

		for ( size_t i = 0; i < ihex_buffer_sz; ++i ) {
				printf( "%x", (int)ihex_buffer[i] );
		}
		printf( "\n" );

		char * upload_error =
			upload_buffer_to_dev( ihex_buffer, ihex_buffer_sz );
		if ( upload_error ) {
				fprintf( stderr
				       , "Unable to upload buffer to device: %s\n", upload_error
				       );
				goto unwind_dev;
		}

		exit_code = EXIT_SUCCESS;
	unwind_dev:

	unwind_file:
		fclose( ihex_file );
	exit_program:
		return exit_code;
}

#define MAX_ERROR_STRING_SZ 256
char * format_error( char * format, ... ) {
		static /*thread_local*/ char error_buffer[MAX_ERROR_STRING_SZ] = {0};
		va_list args;
		va_start( args, format );
		vsnprintf( error_buffer, MAX_ERROR_STRING_SZ, format, args );
		va_end( args );
		return error_buffer;
}

//=== Intel HEX format loading ===//

// According to doc, data field contains 255 (0xFF) max bytes
// Line is 1 ':', 2 bytes count, 4 address, 2 record, 255 max data, 2 checksum
// plus two bytes for new line (/r/n) and one for null-termination
#define IHEX_LINE_MAX_SZ 269

typedef struct {
	ui8    size;
	ui16   addr;
	ui8    type;
	ui8    data[256];
	ui8    checksum;
} ihex_record;

char * read_record_from_line( char * line, ihex_record * record ) {
		size_t length = strlen( line );
		// remove carriage return and new line inserted by fgets
		while ( length > 0 &&
		        (line[length-1] == '\n' || line[length-1] == '\r')
		      ) {
				line[length-1] = 0;
				length -= 1;
		}

		// check that line contains all records and start code BUT data
		// 11 characters total
		if ( length < 11 )    {
				return "Invalid line length";
		}
		if ( line[0] != ':' ) {
				return "Invalid start code";
		}

		int size, addr, type;
		int status = sscanf( line+1, "%2x%4x%2x", &size, &addr, &type );
		if ( status != 3 ) {
				return "Error parsing record header";
		}
		// check that line contains all records and start code AND data
		if ( length != 11+2*size ) {
				return "Invalid line length";
		}
		record->size = (ui8) size;
		record->addr = (ui16)addr;
		record->type = (ui8) type;

		for ( size_t i = 0; i < size; ++i ) {
				int datum;
				if ( sscanf( line+9+2*i, "%2x", &datum ) != 1 ) {
						return "Error parsing record data";
				};
				record->data[i] = (ui8)datum;
		}
		int checksum;
		if ( sscanf( line+9+2*size, "%2x", &checksum ) != 1 ) {
				return "Error parsing record chechsum";
		}
		record->checksum = (ui8)checksum;

		return NULL;
}

bool validate_record( ihex_record * record ) {
		ui8 sum = record->size;
		sum    += record->addr>>8;
		sum    += (ui8)record->addr;
		sum    += record->type;
		for ( size_t i = 0; i < record->size; ++i ) {
				sum += record->data[i];
		}
		sum    += record->checksum;
		return sum == 0;
}

char * load_ihex_buffer( FILE * ihex_file, ui8 * buffer, size_t * buffer_sz ) {
		assert( buffer && buffer_sz && *buffer_sz );

		size_t line_number = 0;
		size_t highest_addr = 0;
		bool last_record_read = false;
		char line_buffer[IHEX_LINE_MAX_SZ] = {0};
		do {
				ihex_record record = {0};

				char * line = fgets( line_buffer, IHEX_LINE_MAX_SZ, ihex_file );
				line_number += 1;
				if ( !line ) {
						if ( feof( ihex_file ) ) {
								if ( last_record_read ) {
										break;   // we're fine! :)
								}
								return "Unexpected end-of-file";
						}
						assert( ferror( ihex_file ) );
						return format_error( "Error reading ihex file (line %zu): %s"
						                   , line_number, strerror( errno )
						                   );
				}
				if ( last_record_read ) {
						return format_error( "Data after last record (line %zu)"
						                   , line_number
						                   );
				}

				char * error = read_record_from_line( line, &record );
				if ( error ) {
						return error;
				}
				bool valid_record = validate_record( &record );
				if ( !valid_record ) {
						return format_error( "Invalid record (line %zu)"
						                   , line_number
						                   );
				}

				switch( record.type ) {
					case 0: {
							size_t end_addr = record.addr + record.size;
							if ( end_addr >= *buffer_sz ) {
									return format_error(
										"Addr too high to upload (line %zu)"
									                   , line_number
									                   );
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
						return format_error( "Only support 8bit ihex (line %zu)"
						                   , line_number
						                   );
					default:
						return format_error( "Invalid record type (line %zu)"
						                   , line_number
						                   );
				}
		} while ( true );
		*buffer_sz = highest_addr;
		return NULL;
}

//=== USB Device Firmware Update upload ===//

char * upload_buffer_to_dev( ui8 * buffer, size_t buffer_sz ) {

		return NULL;
}
