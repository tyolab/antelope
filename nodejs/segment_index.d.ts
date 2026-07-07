export type Metric = 'dot' | 'cosine' | 'l2';

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
}

export interface DocRef { generation: number; docid: number; }
export interface Hit extends DocRef { key: string; score: number; }

export class SegmentIndex {
	constructor(options?: SegmentIndexOptions);
	open(directory: string): void;
	close(): void;
	addDocument(key: string, text: string, vector?: Float32Array | number[], multiVectors?: Float32Array[]): DocRef;
	updateDocument(key: string, text: string, vector?: Float32Array | number[], multiVectors?: Float32Array[]): DocRef;
	deleteDocument(key: string): boolean;
	search(text: string, k: number): Hit[];
	searchVector(vector: Float32Array | number[], k: number): Hit[];
	searchHybrid(text: string | null, vector: Float32Array | number[] | null, k: number): Hit[];
	searchVectorApprox(vector: Float32Array | number[], k: number): Hit[];
	searchHybridApprox(text: string | null, vector: Float32Array | number[] | null, k: number): Hit[];
	searchVectorHnsw(vector: Float32Array | number[], k: number): Hit[];
	searchHybridHnsw(text: string | null, vector: Float32Array | number[] | null, k: number): Hit[];
	searchRerank(queryMultiVectors: Float32Array[], options: { text?: string; vector?: Float32Array | number[]; firstStageN?: number; topK?: number }): Hit[];
	flush(): Promise<void>;
	maintain(): Promise<void>;
	buildSignatures(): Promise<void>;
	buildHnsw(): Promise<void>;
	buildQuantized(): Promise<void>;
	documentCount(): number;
	vectorDimension(): number;
}
