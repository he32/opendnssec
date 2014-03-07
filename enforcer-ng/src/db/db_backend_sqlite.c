#include "db_backend_sqlite.h"

int db_backend_sqlite_connect(db_backend_handle_t* backend_handle, const db_configuration_list_t* configuration) {
	return 0;
}

int db_backend_sqlite_disconnect(db_backend_handle_t* backend_handle) {
	return 0;
}

db_backend_handle_t* db_backend_sqlite_new_handle(void) {
	db_backend_handle_t* backend_handle;

	if ((backend_handle = db_backend_handle_new())) {
		db_backend_handle_set_connect(backend_handle, db_backend_sqlite_connect);
		db_backend_handle_set_disconnect(backend_handle, db_backend_sqlite_disconnect);
	}
	return backend_handle;
}
