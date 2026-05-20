export type TypedError<TCode extends string, TDetails = undefined> = Error & {
  code: TCode;
  details: TDetails;
};

export function newTypedError<TCode extends string, TDetails>(
  code: TCode,
  msg: string,
  details: TDetails,
): TypedError<TCode, TDetails> {
  return Object.assign(new Error(msg), {
    name: code,
    code,
    details: details,
  });
}

export function isTypedError(error: unknown): error is TypedError<string, unknown> {
  return error instanceof Error &&
    "code" in error &&
    typeof error.code === "string" &&
    "details" in error;
}
