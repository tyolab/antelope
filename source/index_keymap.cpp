/*
	INDEX_KEYMAP.CPP
	----------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "index_keymap.h"

/*
	ANT_INDEX_KEYMAP::ANT_INDEX_KEYMAP()
	-----------------------------------
*/
ANT_index_keymap::ANT_index_keymap(const char *directory)
{
this->directory = new char[strlen(directory) + 1];
strcpy(this->directory, directory);
slots_allocated = 1024;
table = new slot[slots_allocated];
memset(table, 0, (size_t)(slots_allocated * sizeof(slot)));
slots_used = 0;
log = NULL;
}

/*
	ANT_INDEX_KEYMAP::~ANT_INDEX_KEYMAP()
	------------------------------------
*/
ANT_index_keymap::~ANT_index_keymap()
{
if (log != NULL)
	fclose(log);
for (long long i = 0; i < slots_allocated; i++)
	if (table[i].key != NULL)
		delete [] table[i].key;
delete [] table;
delete [] directory;
}

/*
	ANT_INDEX_KEYMAP::HASH()
	-----------------------
	FNV-1a 64-bit hash
*/
unsigned long long ANT_index_keymap::hash(const char *key)
{
unsigned long long hash_val = 14695981039346656037ULL;
unsigned long long prime = 1099511628211ULL;

for (const unsigned char *p = (const unsigned char *)key; *p; p++)
	{
	hash_val ^= *p;
	hash_val *= prime;
	}
return hash_val;
}

/*
	ANT_INDEX_KEYMAP::KEY_IS_VALID()
	--------------------------------
	Check: non-NULL, non-empty, no '\t', no '\n'
*/
long ANT_index_keymap::key_is_valid(const char *key)
{
if (key == NULL || key[0] == '\0')
	return 0;
for (const char *p = key; *p; p++)
	{
	if (*p == '\t' || *p == '\n')
		return 0;
	}
return 1;
}

/*
	ANT_INDEX_KEYMAP::FIND_SLOT()
	-----------------------------
	Linear probing: find a slot with matching key, or the first never-used slot
*/
ANT_index_keymap::slot *ANT_index_keymap::find_slot(const char *key)
{
unsigned long long h = hash(key);
long long index = h % slots_allocated;

while (table[index].key != NULL && strcmp(table[index].key, key) != 0)
	{
	index = (index + 1) % slots_allocated;
	}
return &table[index];
}

/*
	ANT_INDEX_KEYMAP::INSERT_NO_LOG()
	---------------------------------
	Grow at 75% load. Copy key on first use of slot. Set generation/docid.
	Does NOT write to log (called by load() and grow()).
*/
void ANT_index_keymap::insert_no_log(const char *key, long long generation, long long docid)
{
/*
	Check if we need to grow: 75% load = slots_used * 4 >= slots_allocated * 3
*/
if (slots_used * 4 >= slots_allocated * 3)
	grow();

slot *s = find_slot(key);
if (s->key == NULL)
	{
	s->key = new char[strlen(key) + 1];
	strcpy(s->key, key);
	slots_used++;
	}
s->generation = generation;
s->docid = docid;
}

/*
	ANT_INDEX_KEYMAP::GROW()
	-----------------------
	Double table; re-insert only live entries (docid >= 0) via insert_no_log.
	Free old keys and table.
*/
void ANT_index_keymap::grow(void)
{
slot *old_table = table;
long long old_allocated = slots_allocated;

slots_allocated *= 2;
table = new slot[slots_allocated];
memset(table, 0, (size_t)(slots_allocated * sizeof(slot)));
slots_used = 0;

for (long long i = 0; i < old_allocated; i++)
	{
	if (old_table[i].key != NULL && old_table[i].docid >= 0)
		{
		insert_no_log(old_table[i].key, old_table[i].generation, old_table[i].docid);
		}
	}

/*
	Free old keys from table before deleting the table itself
*/
for (long long i = 0; i < old_allocated; i++)
	if (old_table[i].key != NULL)
		delete [] old_table[i].key;

delete [] old_table;
}

/*
	READ_KEYMAP_LINE()
	------------------
	Read one logical line into buffer. Returns 1 if a complete line was read
	(a final line at EOF without a trailing newline counts as complete),
	0 at EOF, or -1 if the line was too long for the buffer, in which case
	the rest of the logical line is discarded and the caller must ignore the
	truncated fragment.
*/
static long read_keymap_line(FILE *fp, char *buffer, size_t buffer_size)
{
int character;

if (fgets(buffer, (int)buffer_size, fp) == NULL)
	return 0;

if (strchr(buffer, '\n') == NULL && !feof(fp))
	{
	/*
		Overlong line: throw away the remainder so the next read starts at
		the next logical line, and tell the caller this fragment is garbage.
	*/
	while ((character = fgetc(fp)) != '\n' && character != EOF)
		;	// discard
	return -1;
	}

return 1;
}

/*
	ANT_INDEX_KEYMAP::LOAD()
	-----------------------
	Build path with snprintf. Replay records from log file.
	Lines "A\t..." are add records: parse generation, docid, key.
	Lines "D\t..." are delete records: mark removed (docid = -1).
	Apply bounds/truncation/malformed-record hardening.
*/
ANT_index_keymap *ANT_index_keymap::load(const char *directory)
{
ANT_index_keymap *result = new ANT_index_keymap(directory);
char keymap_path[4096];
FILE *fp;
char line[8192];
long status;

if (snprintf(keymap_path, sizeof(keymap_path), "%s/keymap.log", directory) >= (int)sizeof(keymap_path))
	{
	/*
		Path too long: return fresh map without trying to open log for append
	*/
	return result;
	}

if ((fp = fopen(keymap_path, "r")) == NULL)
	{
	/*
		No log file: open for append and return fresh map
	*/
	result->log = fopen(keymap_path, "a");
	return result;
	}

/*
	Replay the log file
*/
while ((status = read_keymap_line(fp, line, sizeof(line))) != 0)
	{
	if (status != 1)
		{
		/*
			Overlong line: fragment discarded, parse nothing from it
		*/
		continue;
		}

	/*
		Parse the record type
	*/
	if (line[0] == 'A' && line[1] == '\t')
		{
		/*
			Add record: A<TAB>generation<TAB>docid<TAB>key
			Parse generation and docid strictly: strtoll must consume at
			least one digit and stop exactly at the field's tab terminator,
			otherwise the record is malformed and skipped (atoll would
			silently turn garbage into 0, and 0 is a valid docid).
		*/
		char *gen_str = line + 2;
		char *gen_end;
		long long generation = strtoll(gen_str, &gen_end, 10);
		if (gen_end == gen_str || *gen_end != '\t')
			continue;		// malformed generation field: skip record

		char *docid_str = gen_end + 1;
		char *docid_end;
		long long docid = strtoll(docid_str, &docid_end, 10);
		if (docid_end == docid_str || *docid_end != '\t')
			continue;		// malformed docid field: skip record

		/*
			Bound-check generation and docid
		*/
		if (generation <= 0 || generation >= (1LL << 40))
			continue;		// out of range: skip record
		if (docid < 0 || docid >= (1LL << 40))
			continue;		// out of range: skip record

		char *key = docid_end + 1;
		/*
			Remove trailing newline if present
		*/
		char *newline = strchr(key, '\n');
		if (newline != NULL)
			*newline = '\0';

		/*
			Validate the key
		*/
		if (!key_is_valid(key))
			continue;		// invalid key: skip record

		result->insert_no_log(key, generation, docid);
		}
	else if (line[0] == 'D' && line[1] == '\t')
		{
		/*
			Delete record: D<TAB>key
		*/
		char *key = line + 2;
		/*
			Remove trailing newline if present
		*/
		char *newline = strchr(key, '\n');
		if (newline != NULL)
			*newline = '\0';

		/*
			Validate the key
		*/
		if (!key_is_valid(key))
			continue;		// invalid key: skip record

		/*
			Find and mark as deleted (docid = -1)
		*/
		slot *s = result->find_slot(key);
		if (s->key != NULL)
			s->docid = -1;		// mark removed
		}
	}

fclose(fp);

/*
	Open log for append
*/
result->log = fopen(keymap_path, "a");

return result;
}

/*
	ANT_INDEX_KEYMAP::ADD()
	----------------------
	Validate key; insert via insert_no_log; write to log if open.
*/
void ANT_index_keymap::add(const char *key, long long generation, long long docid)
{
if (!key_is_valid(key))
	return;		// no-op: invalid key

insert_no_log(key, generation, docid);

if (log != NULL)
	{
	if (fprintf(log, "A\t%lld\t%lld\t%s\n", generation, docid, key) >= 0)
		fflush(log);
	}
}

/*
	ANT_INDEX_KEYMAP::REMOVE()
	--------------------------
	Validate key; mark docid = -1 if slot in use; write to log if open.
*/
void ANT_index_keymap::remove(const char *key)
{
if (!key_is_valid(key))
	return;		// no-op: invalid key

slot *s = find_slot(key);
if (s->key != NULL)
	s->docid = -1;		// mark removed

if (log != NULL)
	{
	if (fprintf(log, "D\t%s\n", key) >= 0)
		fflush(log);
	}
}

/*
	ANT_INDEX_KEYMAP::FIND()
	-----------------------
	Validate key; if slot is NULL or docid < 0, return 0 (not found).
	Otherwise fill outputs and return 1 (found).
*/
long ANT_index_keymap::find(const char *key, long long *generation, long long *docid)
{
if (!key_is_valid(key))
	return 0;		// not found: invalid key

slot *s = find_slot(key);
if (s->key == NULL || s->docid < 0)
	return 0;		// not found

if (generation != NULL)
	*generation = s->generation;
if (docid != NULL)
	*docid = s->docid;
return 1;		// found
}
