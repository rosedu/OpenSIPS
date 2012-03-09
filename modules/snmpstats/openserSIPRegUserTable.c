/*
 * $Id: openserSIPRegUserTable.c 6024 2009-08-25 14:17:14Z bogdan_iancu $
 *
 * SNMPStats Module 
 * Copyright (C) 2006 SOMA Networks, INC.
 * Written by: Jeffrey Magder (jmagder@somanetworks.com)
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 *
 * History:
 * --------
 * 2006-11-23 initial version (jmagder)
 *
 * Note: this file originally auto-generated by mib2c using
 * mib2c.array-user.conf
 *
 * This file implements the openserSIPRegUserTable.  For a full description of
 * the table, please see the OPENSER-SIP-SERVER-MIB.
 *
 * Understanding this code will be much simpler with the following information:
 *
 * 1) All rows are indexed by an integer user index.  This is different from the
 *    usrloc module, which indexes by strings.  This less natural indexing
 *    scheme was required due to SNMP String index limitations.  (for example,
 *    SNMP has maximum index lengths.)
 *
 * 2) We need a quick way of mapping usrloc indices to our integer indices.  For
 *    this reason a string indexed Hash Table was created, with each entry mapping
 *    to an integer user index. 
 *
 *    This hash table is used by the openserSIPContactTable (the hash table also
 *    maps a user to its contacts), as well as the openserSIPRegUserLookupTable.
 *    The hash table is also used for quick lookups when a user expires. (i.e, it
 *    gives us a more direct reference, instead of having to search the whole
 *    table).
 *
 * 3) We are informed about new/expired users via a callback mechanism from the
 *    usrloc module.  Because of NetSNMP inefficiencies, we had to abstract this
 *    process.  Specifically:
 *
 *    - It can take a long time for the NetSNMP code base to populate a table with
 *      a large number of records. 
 *
 *    - We rely on callbacks for updated user information. 
 *
 *    Clearly, using the SNMPStats module in this situation could lead to some
 *    big performance loses if we don't find another way to deal with this.  The
 *    solution was to use an interprocess communications buffer.  
 *
 *    Instead of adding the record directly to the table, the callback functions
 *    now adds either an add/delete command to the interprocessBuffer.  When an
 *    snmp request is recieved by the SNMPStats sub-process, it will consume
 *    this interprocess buffer, adding and deleting users.  When it is finished,
 *    it can service the SNMP request.  
 *
 *    This doesn't remove the NetSNMP inefficiency, but instead moves it to a
 *    non-critical path.  Such an approach allows SNMP support with almost no
 *    overhead to the rest of OpenSIPS.
 */

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <net-snmp/library/snmp_assert.h>

#include "hashTable.h"
#include "interprocess_buffer.h"
#include "utilities.h"
#include "openserSIPRegUserTable.h"
#include "snmpstats_globals.h"

#include "../../sr_module.h"
#include "../../locking.h"
#include "../usrloc/usrloc.h"

static netsnmp_handler_registration *my_handler = NULL;
static netsnmp_table_array_callbacks cb;

oid    openserSIPRegUserTable_oid[]   = { openserSIPRegUserTable_TABLE_OID };
size_t openserSIPRegUserTable_oid_len = OID_LENGTH(openserSIPRegUserTable_oid);


/* If the usrloc module is loaded, this function will grab hooks into its
 * callback registration function, and add handleContactCallbacks() as the
 * callback for UL_CONTACT_INSERT and UL_CONTACT_EXPIRE.
 *
 * Returns 1 on success, and zero otherwise. */
int registerForUSRLOCCallbacks(void)  
{
	bind_usrloc_t bind_usrloc;
	usrloc_api_t ul;

	bind_usrloc = (bind_usrloc_t)find_export("ul_bind_usrloc", 1, 0);
	if (!bind_usrloc)
	{
		LM_ERR("Can't find ul_bind_usrloc\n");
		goto error;
	}
	if (bind_usrloc(&ul) < 0 || ul.register_ulcb == NULL)
	{
		LM_ERR("Can't bind usrloc\n");
		goto error;
	}

	ul.register_ulcb(UL_CONTACT_INSERT, handleContactCallbacks, NULL);
	
	ul.register_ulcb(UL_CONTACT_EXPIRE, handleContactCallbacks, NULL);
	
	ul.register_ulcb(UL_CONTACT_DELETE, handleContactCallbacks, NULL);

	return 1;

error:
	LM_INFO("failed to register for callbacks with the USRLOC module.");
	LM_INFO("openserSIPContactTable and openserSIPUserTable will be"
			" unavailable");
	return 0;
}


/* Removes an SNMP row indexed by userIndex, and frees the string and index it
 * pointed to. */
void deleteRegUserRow(int userIndex) 
{
	
	openserSIPRegUserTable_context *theRow;

	netsnmp_index indexToRemove;
	oid indexToRemoveOID;
		
	indexToRemoveOID   = userIndex;
	indexToRemove.oids = &indexToRemoveOID;
	indexToRemove.len  = 1;

	theRow = CONTAINER_FIND(cb.container, &indexToRemove);

	/* The userURI is shared memory, the index.oids was allocated from
	 * pkg_malloc(), and theRow was made with the NetSNMP API which uses
	 * malloc() */
	if (theRow != NULL) {
		CONTAINER_REMOVE(cb.container, &indexToRemove);
		pkg_free(theRow->openserSIPUserUri);
		pkg_free(theRow->index.oids);
		free(theRow);
	}

}

/*
 * Adds or updates a user:
 *
 *   - If a user with the name userName exists, its 'number of contacts' count
 *     will be incremented.  
 *   - If the user doesn't exist, the user will be added to the table, and its
 *     number of contacts' count set to 1. 
 */
void updateUser(char *userName) 
{
	int userIndex; 

	aorToIndexStruct_t *newRecord;

	aorToIndexStruct_t *existingRecord = 
		findHashRecord(hashTable, userName, HASH_SIZE);

	/* We found an existing record, so  we need to update its 'number of
	 * contacts' count. */
	if (existingRecord != NULL) 
	{
		existingRecord->numContacts++;
		return;
	}

	/* Make a new row, and insert a record of it into our mapping data
	 * structures */
	userIndex = createRegUserRow(userName);

	if (userIndex == 0) 
	{
		LM_ERR("openserSIPRegUserTable ran out of memory."
				"  Not able to add user: %s", userName);
		return;
	}

	newRecord = createHashRecord(userIndex, userName);
	
	/* If we couldn't create a record in the hash table, then we won't be
	 * able to access this row properly later.  So remove the row from the
	 * table and fail. */
	if (newRecord == NULL) {
		deleteRegUserRow(userIndex);
		LM_ERR("openserSIPRegUserTable was not able to push %s into the hash."
				"  User not added to this table\n", userName);
		return;
	}
	
	/* Insert the new record of the mapping data structure into the hash
	 * table */
	/*insertHashRecord(hashTable,
			createHashRecord(userIndex, userName), 
			HASH_SIZE);*/
	
	insertHashRecord(hashTable,
			newRecord, 
			HASH_SIZE);
}


/* Creates a row and inserts it.  
 *
 * Returns: The rows userIndex on success, and 0 otherwise. */
int createRegUserRow(char *stringToRegister) 
{
	int static index = 0;
	
	index++;

	openserSIPRegUserTable_context *theRow;

	oid  *OIDIndex;
	int  stringLength;

	theRow = SNMP_MALLOC_TYPEDEF(openserSIPRegUserTable_context);

	if (theRow == NULL) {
		LM_ERR("failed to create a row for openserSIPRegUserTable\n");
		return 0;
	}

	OIDIndex = pkg_malloc(sizeof(oid));

	if (OIDIndex == NULL) {
		free(theRow);
		LM_ERR("failed to create a row for openserSIPRegUserTable\n");
		return 0;
	}

	stringLength = strlen(stringToRegister);

	OIDIndex[0] = index;

	theRow->index.len  = 1;
	theRow->index.oids = OIDIndex;
	theRow->openserSIPUserIndex = index;

	theRow->openserSIPUserUri     = (unsigned char*)pkg_malloc(stringLength* sizeof(char));
    if(theRow->openserSIPUserUri== NULL)
    {
        pkg_free(OIDIndex);
		free(theRow);
		LM_ERR("failed to create a row for openserSIPRegUserTable\n");
		return 0;

    }
    memcpy(theRow->openserSIPUserUri, stringToRegister, stringLength);
	
    theRow->openserSIPUserUri_len = stringLength;

	theRow->openserSIPUserAuthenticationFailures = 0;

	CONTAINER_INSERT(cb.container, theRow);

	return index;
}

/* Initializes the openserSIPRegUserTable module.  */
void init_openserSIPRegUserTable(void)
{
	/* Register this table with the master agent */
	initialize_table_openserSIPRegUserTable();

	/* We need to create a default row, so create DefaultUser */
	static char *defaultUser = "DefaultUser";
	
	createRegUserRow(defaultUser);
}



/*
 * Initialize the openserSIPRegUserTable table by defining its contents and how
 * it's structured
 */
void initialize_table_openserSIPRegUserTable(void)
{
	netsnmp_table_registration_info *table_info;

	if(my_handler) {
		snmp_log(LOG_ERR, "initialize_table_openserSIPRegUserTable_hand"
				"ler called again\n");
		return;
	}

	memset(&cb, 0x00, sizeof(cb));

	/* create the table structure itself */
	table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);

	my_handler = 
		netsnmp_create_handler_registration(
			"openserSIPRegUserTable",
			netsnmp_table_array_helper_handler, 
			openserSIPRegUserTable_oid,
			openserSIPRegUserTable_oid_len,
			HANDLER_CAN_RONLY);

	if (!my_handler || !table_info) {
		snmp_log(LOG_ERR, "malloc failed in initialize_table_openser"
				"SIPRegUserTable_handler\n");
		return; /** mallocs failed */
	}

	netsnmp_table_helper_add_index(table_info, ASN_UNSIGNED);

	table_info->min_column = openserSIPRegUserTable_COL_MIN;
	table_info->max_column = openserSIPRegUserTable_COL_MAX;

	cb.get_value = openserSIPRegUserTable_get_value;
	cb.container = netsnmp_container_find("openserSIPRegUserTable_primary:"
			"openserSIPRegUserTable:" "table_container");

	DEBUGMSGTL(("initialize_table_openserSIPRegUserTable",
				"Registering table openserSIPRegUserTable "
				"as a table array\n"));

	netsnmp_table_container_register(my_handler, table_info, &cb, 
			cb.container, 1);
}


/* Handles SNMP GET requests. */
int openserSIPRegUserTable_get_value(
		netsnmp_request_info *request,
		netsnmp_index *item,
		netsnmp_table_request_info *table_info )
{
	/* First things first, we need to consume the interprocess buffer, in
	 * case something has changed. We want to return the freshest data. */
	consumeInterprocessBuffer();

	netsnmp_variable_list *var = request->requestvb;

	openserSIPRegUserTable_context *context = 
		(openserSIPRegUserTable_context *)item;

	switch(table_info->colnum) 
	{

		case COLUMN_OPENSERSIPUSERURI:
			/** SnmpAdminString = ASN_OCTET_STR */
			snmp_set_var_typed_value(var, ASN_OCTET_STR,
				(unsigned char*)context->openserSIPUserUri,
				context->openserSIPUserUri_len );
			break;
	
		case COLUMN_OPENSERSIPUSERAUTHENTICATIONFAILURES:
			/** COUNTER = ASN_COUNTER */
			snmp_set_var_typed_value(var, ASN_COUNTER,
				(unsigned char*)
				&context->openserSIPUserAuthenticationFailures,
				sizeof(
				context->openserSIPUserAuthenticationFailures));
		break;
	
		default: /** We shouldn't get here */
			snmp_log(LOG_ERR, "unknown column in "
				"openserSIPRegUserTable_get_value\n");

			return SNMP_ERR_GENERR;
	}

	return SNMP_ERR_NOERROR;
}


const openserSIPRegUserTable_context *
openserSIPRegUserTable_get_by_idx(netsnmp_index * hdr)
{
	return (const openserSIPRegUserTable_context *)
		CONTAINER_FIND(cb.container, hdr );
}


