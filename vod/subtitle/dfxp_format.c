#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#define DFXP_PREFIX "<tt"
#define DFXP_XML_PREFIX1 "<?xml"
#define DFXP_XML_PREFIX2 "<xml"

#define DFXP_DURATION_ESTIMATE_NODES (10)
#define DFXP_MAX_STACK_DEPTH (10)
#define DFXP_FRAME_RATE (30)

#define DFXP_ELEMENT_P (u_char*)"p"
#define DFXP_ELEMENT_BR (u_char*)"br"
#define DFXP_ELEMENT_SPAN (u_char*)"span"
#define DFXP_ELEMENT_DIV (u_char*)"div"

#define DFXP_ATTR_BEGIN (u_char*)"begin"
#define DFXP_ATTR_END (u_char*)"end"
#define DFXP_ATTR_DUR (u_char*)"dur"

static vod_status_t
dfxp_reader_init(
	request_context_t* request_context,
	vod_str_t* buffer,
	size_t max_metadata_size,
	void** ctx)
{
	u_char* p = buffer->data;

	if (vod_strncmp(p, UTF8_BOM, sizeof(UTF8_BOM) - 1) == 0)
	{
		p += sizeof(UTF8_BOM) - 1;
	}

	if ((vod_strncmp(p, (u_char*)DFXP_XML_PREFIX1, sizeof(DFXP_XML_PREFIX1) - 1) == 0 ||
		vod_strncmp(p, (u_char*)DFXP_XML_PREFIX2, sizeof(DFXP_XML_PREFIX2) - 1) == 0))
	{
		// vod_strstrn requires an extra -1
		if (vod_strstrn(p, DFXP_PREFIX, sizeof(DFXP_PREFIX) - 1 - 1) == NULL)
		{
			return VOD_NOT_FOUND;
		}
	}
	else if (vod_strncmp(p, (u_char*)DFXP_PREFIX, sizeof(DFXP_PREFIX) - 1) != 0)
	{
		return VOD_NOT_FOUND;
	}

	return subtitle_reader_init(
		request_context,
		ctx);
}

// a simplified version of xmlGetProp that does not copy memory, but is also limited to a single child
static xmlChar* 
dfxp_get_xml_prop(xmlNode* node, xmlChar* name)
{
	xmlAttrPtr prop;

	prop = xmlHasProp(node, name);
	if (prop == NULL ||
		prop->children == NULL ||
		prop->children->next != NULL ||
		(prop->children->type != XML_TEXT_NODE && prop->children->type != XML_CDATA_SECTION_NODE))
	{
		return NULL;
	}

	return prop->children->content;
}

static int64_t 
dfxp_parse_timestamp(u_char* ts)
{
	u_char* p = ts;
	int64_t frames;
	int64_t num = 0;
	int64_t den;
	int64_t mul;
	
	// Note: according to spec, hours must be at least 2 digits, but some samples have only one
	//		so this is not enforced
	if (!isdigit(*p))
	{
		return -1;
	}

	do
	{
		num = num * 10 + (*p++ - '0');
	} while (isdigit(*p));

	if (*p == ':')
	{
		// clock time
		p++;	// skip the :
		
		// minutes / seconds
		if (!isdigit(p[0]) || !isdigit(p[1]) ||
			p[2] != ':' ||
			!isdigit(p[3]) || !isdigit(p[4]))
		{
			return -1;
		}
		
		num = num * 3600 + 								// hours
			((p[0] - '0') * 10 + (p[1] - '0')) * 60 +	// min
			((p[3] - '0') * 10 + (p[4] - '0'));			// sec
		p += 5;
		
		switch (*p)
		{
		case '\0':
			return num * 1000;

		case '.':
			// fraction
			p++;	// skip the .
			if (!isdigit(*p))
			{
				return -1;
			}
			
			den = 1;
			do
			{
				num = num * 10 + (*p++ - '0');
				den *= 10;
			} while (isdigit(*p));
			
			if (*p != '\0')
			{
				return -1;
			}

			return (num * 1000) / den;
			
		case ':':
			// frames
			p++;	// skip the :
			if (!isdigit(*p))
			{
				return -1;
			}
			
			frames = 0;
			do
			{
				frames = frames * 10 + (*p++ - '0');
			} while (isdigit(*p));
			
			if (*p != '\0')
			{
				return -1;
			}

			return (num * 1000) + (frames * 1000) / DFXP_FRAME_RATE;
		}
	}
	else
	{
		// offset time
		den = 1;
		if (*p == '.')
		{
			// fraction
			p++;	// skip the .
			if (!isdigit(*p))
			{
				return -1;
			}
			do
			{
				num = num * 10 + (*p++ - '0');
				den *= 10;
			} while (isdigit(*p));
		}
		
		// metric
		switch (*p)
		{
		case 'h':
			mul = 3600000;
			break;
			
		case 'm':
			if (p[1] == 's')
			{
				mul = 1;
				p++;
			}
			else
			{
				mul = 60000;
			}
			break;
			
		case 's':
			mul = 1000;
			break;
			
		case 'f':
			mul = 1000;
			den *= DFXP_FRAME_RATE;
			break;
			
		default:
			return -1;
		}
		
		if (p[1] != '\0')
		{
			return -1;
		}
		
		return (num * mul) / den;
	}

	return -1;
}

typedef struct{
	int64_t start_time;
	int64_t end_time;
} dfxp_timestamp_t;

static int
dfxp_parse_timestamp0(xmlNode* node, xmlChar* name, int64_t* ts)
{
	*ts = -1;
	xmlChar* attr = dfxp_get_xml_prop(node, name);
	if (attr == NULL)
	{
		return 0;
	}
	*ts =  dfxp_parse_timestamp(attr);
	return *ts >= 0;
}

static int
dfxp_extract_time(xmlNode* node, dfxp_timestamp_t* t, int try_end_only)
{
	if (dfxp_parse_timestamp0(node, DFXP_ATTR_END, &t->end_time) && try_end_only)
	{
		return 1; // wanted end only
	}

	if (dfxp_parse_timestamp0(node, DFXP_ATTR_BEGIN, &t->start_time) && !try_end_only)
	{
		return 1; // wanted start, end
	}

	// need to look at duration, but only if start exists
	if (t->start_time < 0 || !dfxp_parse_timestamp0(node, DFXP_ATTR_DUR, &t->end_time))
	{
		return 0; // either dur or start doesn't exist
	}
	t->end_time += t->start_time;
	return 1;
}

static int64_t
dfxp_clamp(int64_t v, int64_t lo, int64_t hi)
{
	if (v < lo)
	{
		return lo;
	}
	if (v > hi)
	{
		return hi;
	}
	return v;
}

static uint64_t
dfxp_get_duration(xmlDoc *doc)
{
	xmlNode* node_stack[DFXP_MAX_STACK_DEPTH];
	xmlNode* cur_node;
	xmlNode temp_node;
	unsigned node_stack_pos = 0;
	int nodes_left = DFXP_DURATION_ESTIMATE_NODES;
	int64_t result = 0;
	dfxp_timestamp_t ts = {0};

	for (cur_node = xmlDocGetRootElement(doc); ; cur_node = cur_node->prev)
	{
		// traverse the tree dfs order (reverse child order)
		if (cur_node == NULL)
		{
			if (node_stack_pos <= 0)
			{
				break;
			}

			cur_node = node_stack[--node_stack_pos];
			continue;
		}

		if (cur_node->type != XML_ELEMENT_NODE)
		{
			continue;
		}

		// timestamp information can be inside a div tag too
		if (vod_strcmp(cur_node->name, DFXP_ELEMENT_DIV) == 0)
		{
			dfxp_extract_time(cur_node, &ts, 1);
			if (ts.end_time > result)
			{
				result = ts.end_time;
			}
		}

		// recurse into non-p nodes
		if (vod_strcmp(cur_node->name, DFXP_ELEMENT_P) != 0)
		{
			if (cur_node->last == NULL || 
				node_stack_pos >= vod_array_entries(node_stack))
			{
				continue;
			}

			node_stack[node_stack_pos++] = cur_node;
			temp_node.prev = cur_node->last;
			cur_node = &temp_node;
			continue;
		}

		// timestamp information can be inside a p tag
		dfxp_extract_time(cur_node, &ts, 1);
		if (ts.end_time > result)
		{
			result = ts.end_time;
		}

		nodes_left--;
		if (nodes_left <= 0)
		{
			break;
		}
	}
	
	return result;
}

static void
dfxp_strip_new_lines(u_char* buf, size_t n)
{
	u_char* end;
	u_char* p;

	end = buf + n;

	for (p = buf; p < end; p++)
	{
		if (*p == CR || *p == LF)
		{
			*p = ' ';
		}
	}
}

// copied from ngx_http_xslt_sax_error
static void vod_cdecl
dfxp_xml_sax_error(void *data, const char *msg, ...)
{
	xmlParserCtxtPtr ctxt = data;
	request_context_t* request_context;
	va_list args;
	u_char buf[VOD_MAX_ERROR_STR];
	size_t n;

	request_context = ctxt->sax->_private;

	buf[0] = '\0';

	va_start(args, msg);
	n = (size_t)vsnprintf((char *)buf, VOD_MAX_ERROR_STR, msg, args);
	va_end(args);

	while (--n && (buf[n] == CR || buf[n] == LF)) { /* void */ }

	dfxp_strip_new_lines(buf, n);

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"dfxp_xml_sax_error: libxml2 error: %*s", n + 1, buf);
}

static void vod_cdecl
dfxp_xml_schema_error(void *data, const char *msg, ...)
{
	xmlParserCtxtPtr ctxt = data;
	request_context_t* request_context;
	va_list args;
	u_char buf[VOD_MAX_ERROR_STR];
	size_t n;

	request_context = ctxt->sax->_private;

	buf[0] = '\0';

	va_start(args, msg);
	n = (size_t)vsnprintf((char *)buf, VOD_MAX_ERROR_STR, msg, args);
	va_end(args);

	while (--n && (buf[n] == CR || buf[n] == LF)) { /* void */ }

	dfxp_strip_new_lines(buf, n);

	vod_log_error(VOD_LOG_WARN, request_context->log, 0,
		"dfxp_xml_schema_error: libxml2 error: %*s", n + 1, buf);
}

static void
dfxp_free_xml_doc(void *data)
{
	xmlFreeDoc(data);
}

static vod_status_t
dfxp_parse(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* source,
	size_t metadata_part_count,
	media_base_metadata_t** result)
{
	vod_pool_cleanup_t *cln;
	xmlParserCtxtPtr ctxt;
	xmlDoc *doc;

	// parse the xml
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dfxp_parse: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	ctxt = xmlCreateDocParserCtxt(source->data);
	if (ctxt == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dfxp_parse: xmlCreateDocParserCtxt failed");
		return VOD_ALLOC_FAILED;
	}

	xmlCtxtUseOptions(ctxt, XML_PARSE_RECOVER | XML_PARSE_NOWARNING | XML_PARSE_NONET);
	
	ctxt->sax->setDocumentLocator = NULL;
	ctxt->sax->error = dfxp_xml_sax_error;
	ctxt->sax->fatalError = dfxp_xml_sax_error;
	ctxt->vctxt.error = dfxp_xml_schema_error;
	ctxt->sax->_private = request_context;

	if (xmlParseDocument(ctxt) != 0 ||
		ctxt->myDoc == NULL ||
		(!ctxt->wellFormed && !ctxt->recovery))
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dfxp_parse: xml parsing failed");
		if (ctxt->myDoc != NULL)
		{
			xmlFreeDoc(ctxt->myDoc);
		}
		xmlFreeParserCtxt(ctxt);
		return VOD_BAD_DATA;
	}

	doc = ctxt->myDoc;
	ctxt->myDoc = NULL;

	xmlFreeParserCtxt(ctxt);

	cln->handler = dfxp_free_xml_doc;
	cln->data = doc;

	return subtitle_parse(
		request_context,
		parse_params,
		source,
		doc,
		dfxp_get_duration(doc),
		metadata_part_count,
		result);
}

static u_char* 
dfxp_append_string(u_char* p, u_char* s)
{
	// Note: not using strcpy as it would require an extra strlen to get the end position
	while (*s)
	{
		*p++ = *s++;
	}
	return p;
}

static u_char* 
dfxp_fake_append_string(u_char* p, u_char* s)
{
	return p + vod_strlen(s);
}

typedef struct{
	char* id;
	struct flag{
		int decoration;
		int text;
		int display;
	} flag;
} style;

typedef struct{
	char* id;
	style style;
} region;

enum decoration_kind {DECO_BOLD, DECO_ITALIC, DECO_UNDERLINE};
#define	DECO_SET_BOLD	(1<<DECO_BOLD)
#define	DECO_SET_ITALIC	(1<<DECO_ITALIC)
#define	DECO_SET_UNDERLINE	(1<<DECO_UNDERLINE)

static struct{char *name, *attr; char *tag[2];} decorationtab[] = {
	{"bold",      "fontWeight",     {"<b>","</b>"}},
	{"italic",    "fontStyle",      {"<i>","</i>"}},
	{"underline", "textDecoration", {"<u>","</u>"}},
	{NULL, NULL, {NULL, NULL}},
};

// textalign and displayalign are rulesets on how we convert between
// dfxp tags to webvtt values.
enum textalign_kind {TA_DEFAULT, TA_START, TA_CENTER, TA_END, TA_LEFT, TA_RIGHT, TA_NULL};
	[TA_DEFAULT] = {"", "textAlign", " "},
	[TA_START]   = {"start", "textAlign",  " position:15% align:start "},
	[TA_CENTER]  = {"center", "textAlign", " position:50% align:middle"},
	[TA_END]     = {"end", "textAlign",    " size:100% position:85% align:end"},
	[TA_LEFT]    = {"left", "textAlign",   " position:15% align:start"},
	[TA_RIGHT]   = {"right", "textAlign",  " size:100% position:85% align:end"},
	[TA_NULL] = {NULL, NULL, NULL},
};

enum displayalign_kind {DA_DEFAULT, DA_BEFORE, DA_CENTER, DA_AFTER, DA_NULL};
static struct{char *name, *attr, *vtt;} displayaligntab[] = {
	[DA_DEFAULT] = {"", "displayAlign", " "},
	[DA_BEFORE]= {"before", "displayAlign", " line:10%"},
	[DA_CENTER]= {"center", "displayAlign", " line:50%"},
	[DA_AFTER]= {"after", "displayAlign", " line:100%"},
	[DA_NULL]= {NULL, NULL, NULL},
};

// Each region imports the "defaultSpeaker" style, but that's just bold text. This means the three
// regions are all bold, center-weighted text, differing only by display alignment. The bold text is
// technically a property of the hard-coded defaultSpeaker region; we set it in the style for simplicity.
// TODO looks like `id` and `style` are referenced later so should declare here?
static region regiontab[] = {
	{"lowerThird",  {"defaultSpeaker", {DECO_SET_BOLD, TA_CENTER, DA_AFTER}}},	
	{"middleThird", {"defaultSpeaker", {DECO_SET_BOLD, TA_CENTER, DA_CENTER}}},
	{"upperThird",  {"defaultSpeaker", {DECO_SET_BOLD, TA_CENTER, DA_BEFORE}}},
	{NULL, {NULL, {0, 0, 0}}},
};

static int
dfxp_has_attr_value(xmlNode* n, char* name, char* value)
{
	xmlChar* attr = dfxp_get_xml_prop(n, (u_char *) name);
	return (attr != NULL) && vod_strcmp(attr, (u_char *) value) == 0;
}

/***

// NOTE:(as) these are very specific to our usecase [for PoC purposes only]
static style style_defaults[] = {
	{"defaultSpeaker", DECO_SET_BOLD, 0, 0},
	{"block", 0, 0, 0},
	{"rollup", 0, 0, 0},
	{NULL},
};


// dfxp_style_merge merges dst with src, modifying dst
// and returning it.
//
// Given two styles, x and y, define x * y:
// x * y = style{ x.decoration | y.decoration,  x.align = y.align }
static style*
dfxp_style_merge(style* dst, style* src)
{
	dst->decoration |= src->decoration;
	if (src->align.display)
		dst->align.display = src->align.display;
	if (src->align.text)
		dst->align.text = src->align.text;
	return dst;
}

****/

static char* syle_containers[] = {
	"p",
	"div",
	"region",
	"span",
	"body",
	NULL,
};

// dfxp_can_contain_style returns true if the element might contain style information
static int
dfxp_can_contain_style(xmlNode* n)
{
	for (int i = 0; syle_containers[i] != NULL; i++)
		if (vod_strcmp(n->name, (u_char *) syle_containers[i]) == 0)
			return 1;

	return 0;
}

// dfxp_add_textflags ORs any decorations flags founds in the xmlNode
// in the flag bits and returns flag:
static char
dfxp_add_textflags(xmlNode* n, char flag)
{ 
	for (int i = 0; decorationtab[i].name != NULL; i++)
		flag |= dfxp_has_attr_value(n, decorationtab[i].attr, decorationtab[i].name) << i;

	return flag;
}

// dfxp_parse_style extracts alignment, display, and decoration
// attributes and applies it to the style. Its search method is
// lazy per category. So the first display alignment, text alignment
// and decoration will be the one used.
//
// it searches the pre-declared static style tables
static style*
dfxp_parse_style(xmlNode* n, style *s)
{ 
	for (int i = 0; regiontab[i].id != NULL; i++) {
		if (dfxp_has_attr_value(n, "region", regiontab[i].id)){
			// TODO(as): clearly, we can check if it has a region attr at all
			// so we dont have to run this loop over and over again if that
			// tag doesn't exist
			
			// TODO(as) should the parent merge with the region? or just
			// the level nodes and children?
			*s = regiontab[i].style;
			break;
		}
	}

	for (int i = 0; textaligntab[i].name != NULL; i++){
		if (dfxp_has_attr_value(n, textaligntab[i].attr, textaligntab[i].name)){
			s->flag.text = i;
			break;
		}
	}

	for (int i = 0; displayaligntab[i].name != NULL; i++) {
		if (dfxp_has_attr_value(n, displayaligntab[i].attr, displayaligntab[i].name)){
			s->flag.display = i;
			break;
		}
	}

	s->flag.decoration |= dfxp_add_textflags(n, s->flag.decoration);

	return s;
}

// dfxp_append_tag applies the HTML-like text decoration
// tag to p, according to the difference between the flag bits
// and the parent flag bits, and returns p. 
//
// If close is non-zero, close tags (i.e., </b>) are applied instead
// of open tags.
//
static u_char* 
dfxp_append_tag(u_char* p, char flag, char parentflag, int close, u_char* appendfunc (u_char*, u_char*))
{
	// NOTE(as): we only want to append an open or close tag here
	// if the child has something the parent doesn't. This ensures
	// we don't have redundant tags across nodes and their ancestors.
	flag &= (flag ^ parentflag);

	if (flag == 0)
	{
		return p;
	}

	if (close == 0)
	{
		for (int i = 0; decorationtab[i].name != NULL; i++)
		{
			if (flag & (1<<i))
			{
				p = appendfunc(p, (u_char *) decorationtab[i].tag[0]);
			}
		}
		return p;
	}

	// traverse it in reverse order so they look <b><i>like this</i></b>
	for (int i = vod_array_entries(decorationtab) - 1; i >= 0; i--)
	{
		if (flag & (1<<i))
		{
			p = appendfunc(p, (u_char *) decorationtab[i].tag[1]);
		}
	}
	return p;
}

// dfxp_append_style applies the alignments suffix text
// this should be done after the cue.
//
// 00:00:00:000 -> 00:00:00:000 %s
static u_char* 
dfxp_append_style(u_char* p, style *s)
{
	if (s->flag.text)
		p = dfxp_append_string(p, (u_char *) textaligntab[s->flag.text].vtt);
	if (s->flag.display)
		p = dfxp_append_string(p, (u_char *) displayaligntab[s->flag.display].vtt);
	return p;
}

static u_char* 
dfxp_append_text_content(xmlNode* node, u_char* p, char flag, u_char* appendfunc (u_char*, u_char*))
{
	struct{
		xmlNode* node;
		char flag;
	} stack[DFXP_MAX_STACK_DEPTH] = {0};
	int depth = 0;

	char lflag = 0;	// local to <span> tags

	for (;;)
	{
		// traverse the tree dfs order
		if (node == NULL){
			if (depth == 0)
				break;

			node = stack[--depth].node;

			p = dfxp_append_tag(p, lflag, stack[depth].flag, 1, appendfunc);  /* close tag */
			lflag = stack[depth].flag;

			continue;
		}

		switch (node->type)
		{
		case XML_TEXT_NODE:
		case XML_CDATA_SECTION_NODE:
			p = appendfunc(p, node->content);
			break;
		case XML_ELEMENT_NODE:
			if (vod_strcmp(node->name, DFXP_ELEMENT_BR) == 0)
			{
				p = appendfunc(p, (u_char*) "\n");
				break;
			}

			if (vod_strcmp(node->name, DFXP_ELEMENT_SPAN) != 0 ||
				node->children == NULL ||
				depth >= DFXP_MAX_STACK_DEPTH)
			{
				break;
			}
			

			stack[depth].node = node->next;
			stack[depth].flag = lflag;

			lflag = dfxp_add_textflags(node, flag);

			p = dfxp_append_tag(p, lflag, stack[depth].flag, 0, appendfunc);  /* open tag */

			depth++;
			node = node->children;
			continue;

		default:
			break;
		}

		node = node->next;
	}

	return p;
}

#define DECORATION_SCRATCH_SPACE (64)

static vod_status_t
dfxp_get_frame_body(request_context_t* ctx, xmlNode* node, style *style, vod_str_t* result)
{
	size_t alloc_size = (size_t) dfxp_append_text_content(node, 0, style->flag.decoration, dfxp_fake_append_string);
	if (alloc_size == 0) {
		return VOD_NOT_FOUND;
	}
	alloc_size += DECORATION_SCRATCH_SPACE;

	u_char* start = vod_alloc(ctx->pool, alloc_size);
	if (start == NULL){
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, ctx->log, 0,"dfxp_get_frame_body: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	u_char* end = dfxp_append_style(start, style);

	*end++ = ' ';
	u_char* textstart = end;
	end = dfxp_append_text_content(node, end, style->flag.decoration, dfxp_append_string);

	// After inserting the cue, seek to the end of the whitespace, converting the path of whitespace
	// to space characters, and overwrite the last one with a newline.
	while (textstart < end && isspace(*textstart)) {
		*textstart++ = ' ';
	}
	textstart[-1] = '\n';

	*end++ = '\n';
	*end++ = '\n';

	if ((size_t)(end - start) > alloc_size) {
		vod_log_error(VOD_LOG_ERR, ctx->log, 0,"dfxp_get_frame_body: result length %uz exceeded allocated length %uz",(size_t)(end - start + 2), alloc_size);
		return VOD_UNEXPECTED;
	}

	result->data = start;
	result->len = end - start;

	return VOD_OK;
}

static vod_status_t
dfxp_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	struct segmenter_conf_s* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result)
{
	subtitle_base_metadata_t* metadata = vod_container_of(base, subtitle_base_metadata_t, base);
	media_track_t* track = base->tracks.elts;
	input_frame_t* cur_frame = NULL;
	vod_array_t frames;
	vod_str_t* header = &track->media_info.extra_data;
	uint64_t base_time;
	uint64_t clip_to;
	uint64_t start;
	uint64_t end;
	int64_t last_start_time = 0;

	dfxp_timestamp_t t = {0};

	
	xmlNode* cur_node;
	xmlNode* last_div = NULL;
	xmlNode temp_node;
	vod_str_t text;
	vod_status_t rc;

	struct{
		xmlNode* node;
		style style;
	} stack[DFXP_MAX_STACK_DEPTH] = { 0 };
	int depth = 0;

	style style = {0};


	// initialize the result
	vod_memzero(result, sizeof(*result));
	result->first_track = track;
	result->last_track = track + 1;
	result->track_count[MEDIA_TYPE_SUBTITLE] = 1;
	result->total_track_count = 1;

	header->len = sizeof(WEBVTT_HEADER_NEWLINES) - 1;
	header->data = (u_char*)WEBVTT_HEADER_NEWLINES;
	
	if ((parse_params->parse_type & PARSE_FLAG_FRAMES_ALL) == 0)
	{
		return VOD_OK;
	}

	// init the frames array
	if (vod_array_init(&frames, request_context->pool, 5, sizeof(*cur_frame)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dfxp_parse_frames: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	// get the start / end offsets
	start = parse_params->range->start + parse_params->clip_from;

	if ((parse_params->parse_type & PARSE_FLAG_RELATIVE_TIMESTAMPS) != 0)
	{
		base_time = start;
		clip_to = parse_params->range->end - parse_params->range->start;
		end = clip_to;
	}
	else
	{
		base_time = parse_params->clip_from;
		clip_to = parse_params->clip_to;
		end = parse_params->range->end;		// Note: not adding clip_from, since end is checked after the clipping is applied to the timestamps
	}

	for (cur_node = xmlDocGetRootElement(metadata->context); ; cur_node = cur_node->next)
	{
		// traverse the tree dfs order
		if (cur_node == NULL)
		{
			if (depth == 0)
			{
				if (cur_frame != NULL)
				{
					cur_frame->duration = t.end_time - t.start_time;
					track->total_frames_duration = t.end_time - track->first_frame_time_offset;
				}
				break;
			}

			depth--;
			cur_node = stack[depth].node;
			style = stack[depth].style;
			
			if (cur_node == last_div)
			{
				last_div = NULL;
			}
			
			// TODO(as): Check whether cur_node matches the node in the style
			// stack, and pop it if it does, reverting to the previous style
			continue;
		}

		if (cur_node->type != XML_ELEMENT_NODE)
		{
			continue;
		}

		// start with the parent node's style information, then parse
		// additional data from the current node if possible
		if (depth > 0){
			style = stack[depth-1].style;
		}
		if (dfxp_can_contain_style(cur_node))
		{
			dfxp_parse_style(cur_node, &style);
		}

		if (vod_strcmp(cur_node->name, DFXP_ELEMENT_P) != 0)
		{
			if (cur_node->children == NULL || depth == DFXP_MAX_STACK_DEPTH)
			{
				continue;
			}

			// NOTE(as): It's a div, so save it in case the p-tag doesn't have the time inside 
			if (vod_strcmp(cur_node->name, DFXP_ELEMENT_DIV) == 0)
			{
				last_div = cur_node;
			}

			stack[depth].style = style;
			stack[depth].node = cur_node;
			depth++;
			temp_node.next = cur_node->children;
			cur_node = &temp_node;
			continue;
		}

		// handle p element or the last-visited div
		if (!dfxp_extract_time(cur_node, &t, 0))
		{
			if (last_div == NULL || !dfxp_extract_time(last_div, &t, 0))
			{
				continue;
			}
		}

		if ((uint64_t)t.end_time < start)
		{
				track->first_frame_index++;
				continue;
		}

		if (t.start_time >= t.end_time)
		{
			continue;
		}

		// apply clipping
		t.start_time = dfxp_clamp(t.start_time - base_time, 0, clip_to);
		t.end_time = dfxp_clamp(t.end_time - base_time, 0, clip_to);

		rc = dfxp_get_frame_body(request_context, cur_node->children, &style, &text);

		switch (rc)
		{
		case VOD_NOT_FOUND:
			continue;

		case VOD_OK:
			break;

		default:
			return rc;
		}

		// adjust the duration of the previous frame
		if (cur_frame != NULL)
		{
			cur_frame->duration = t.start_time - last_start_time;
		}
		else
		{
			track->first_frame_time_offset = t.start_time;
		}

		if ((uint64_t)t.start_time >= end)
		{
			track->total_frames_duration = t.start_time - track->first_frame_time_offset;
			break;
		}

		// add the frame
		cur_frame = vod_array_push(&frames);
		if (cur_frame == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"dfxp_parse_frames: vod_array_push failed");
			return VOD_ALLOC_FAILED;
		}

		cur_frame->offset = (uintptr_t)text.data;
		cur_frame->size = text.len;
		cur_frame->pts_delay = t.end_time - t.start_time;
		cur_frame->key_frame = 0;
		track->total_frames_size += cur_frame->size;

		last_start_time = t.start_time;
	}

	track->frame_count = frames.nelts;
	track->frames.first_frame = frames.elts;
	track->frames.last_frame = track->frames.first_frame + frames.nelts;

	return VOD_OK;
}

void
dfxp_init_process()
{
	xmlInitParser();
}

void
dfxp_exit_process()
{
	xmlCleanupParser();
}

media_format_t dfxp_format = {
	FORMAT_ID_DFXP,
	vod_string("dfxp"),
	dfxp_reader_init,
	subtitle_reader_read,
	NULL,
	NULL,
	dfxp_parse,
	dfxp_parse_frames,
};
