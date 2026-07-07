export type Metric = 'dot' | 'cosine' | 'l2';

/**
 * Declares an attribute filter schema.  Each value is a type token:
 *   'int64' | 'string' | 'bool'  (scalar),
 *   'int64[]' | 'string[]'        (multi-valued).
 * ('bool[]' is not allowed -- bool is scalar only.)
 */
export type AttributeSchema = Record<string, 'int64' | 'string' | 'bool' | 'int64[]' | 'string[]'>;

/**
 * A per-document attribute payload for addDocument/updateDocument: each field
 * carries a scalar or (for multi-valued fields) an array of values.
 */
export type AttributeValues = Record<string, number | string | boolean | number[] | string[]>;

/** JSON filter predicate tree (translated to an engine filter). */
export type Filter =
	| { and: Filter[] }
	| { or: Filter[] }
	| { not: Filter }
	| { eq: Record<string, string | number | boolean> }
	| { range: Record<string, { gte?: number; gt?: number; lte?: number; lt?: number }> }
	| { in: Record<string, (string | number)[]> };

export interface SearchOptions { filter?: Filter; }

export interface SegmentIndexOptions {
	dimension?: number;
	metric?: Metric;
	flushThreshold?: number;
	mergeFactor?: number;
	tombstoneRatio?: number;
	autoMaintain?: boolean;
	durable?: boolean;
	walFsync?: boolean;
	globalStats?: boolean;
	approximate?: { bits?: number; multiplier?: number };
	hnsw?: { M?: number; efConstruction?: number; efSearch?: number };
	quantize?: 'int8' | 'replace' | 'exact' | { mode: 'replace' | 'exact' };
	rerank?: { dimension: number; quantize?: 'int8' | 'float' };
	attributes?: AttributeSchema;
}

export interface WriteOptions { attributes?: AttributeValues; payload?: Buffer | string; }

export interface DocRef { generation: number; docid: number; }
export interface Hit extends DocRef { key: string; score: number; payload?: Buffer; }

export class SegmentIndex {
	constructor(options?: SegmentIndexOptions);
	open(directory: string): void;
	close(): void;
	addDocument(key: string, text: string, vector?: Float32Array | number[], multiVectors?: Float32Array[], options?: WriteOptions): DocRef;
	updateDocument(key: string, text: string, vector?: Float32Array | number[], multiVectors?: Float32Array[], options?: WriteOptions): DocRef;
	deleteDocument(key: string): boolean;
	search(text: string, k: number, options?: SearchOptions): Hit[];
	searchVector(vector: Float32Array | number[], k: number, options?: SearchOptions): Hit[];
	searchHybrid(text: string | null, vector: Float32Array | number[] | null, k: number, options?: SearchOptions): Hit[];
	searchVectorApprox(vector: Float32Array | number[], k: number, options?: SearchOptions): Hit[];
	searchHybridApprox(text: string | null, vector: Float32Array | number[] | null, k: number, options?: SearchOptions): Hit[];
	searchVectorHnsw(vector: Float32Array | number[], k: number, options?: SearchOptions): Hit[];
	searchHybridHnsw(text: string | null, vector: Float32Array | number[] | null, k: number, options?: SearchOptions): Hit[];
	searchRerank(queryMultiVectors: Float32Array[], options: { text?: string; vector?: Float32Array | number[]; firstStageN?: number; topK?: number; filter?: Filter }): Hit[];
	flush(): Promise<void>;
	maintain(): Promise<void>;
	buildSignatures(): Promise<void>;
	buildHnsw(): Promise<void>;
	buildQuantized(): Promise<void>;
	documentCount(): number;
	vectorDimension(): number;
}
